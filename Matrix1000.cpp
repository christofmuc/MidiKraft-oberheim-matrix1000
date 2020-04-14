/*
   Copyright (c) 2020 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#include "Matrix1000.h"

#include "SynthParameterDefinition.h"
#include "Synth.h"

#include <boost/format.hpp>

#include "Matrix1000Patch.h"
#include "Matrix1000_GlobalSettings.h"

//#include "Matrix1000BCR.h"
//#include "BCR2000.h"

#include "MidiHelpers.h"

#include <set>
//#include "BCR2000_Presets.h"

namespace midikraft {

	enum MIDI_ID {
		OBERHEIM = 0x10,
		MATRIX6_1000 = 0x06
	};

	enum MIDI_COMMAND {
		SINGLE_PATCH_DATA = 0x01,
		REQUEST_DATA = 0x04,
		SET_BANK = 0x0a,
		PARAMETER_EDIT = 0x0b,
		BANK_UNLOCK = 0x0c,
		SINGLE_PATCH_TO_EDIT_BUFFER = 0x0d,
		STORE_EDIT_BUFFER = 0x0e,
	};

	// Definition for the "unused" data bytes inside the sysex of the Matrix 1000
	// These unused bytes need to be blanked for us to compare patches in order to detect duplicates
	std::vector<Range<int>> kMatrix1000BlankOutZones = {
		{0, 8} // This is the ASCII name, 8 character. The Matrix1000 will never display it, but I think a Matrix6 will
	};

	struct Matrix1000GlobalSettingDefinition {
		int sysexIndex;
		TypedNamedValue typedNamedValue;
		bool isTwosComplement;
		int displayOffset;
	};

	// Table of global settings. What I left out was the "group enabled" array for one bit per patch to specify if it is deemed to be group-worthy.
	std::vector<Matrix1000GlobalSettingDefinition> kMatrix1000GlobalSettings = {
		{ 34, { "Master Transpose", "Tuning", Value(), ValueType::Integer, -24, 24 }, true },
		{ 8, { "Master Tune", "Tuning", Value(), ValueType::Integer, -32, 32 }, true },
		{ 11, { "MIDI Basic Channel", "MIDI", Value(), ValueType::Integer, 1, 16 }, false, 1 /* Make it one based */},
		{ 12, { "MIDI OMNI Mode Enable", "MIDI", Value(), ValueType::Bool, 0, 1 } },
		{ 13, { "MIDI Controllers enable", "MIDI", Value(), ValueType::Bool, 0, 1 } },
		{ 14, { "MIDI Patch Changes Enable", "MIDI", Value(), ValueType::Bool, 0, 1 } },
		{ 17, { "MIDI Pedal 1 Controller", "MIDI", Value(), ValueType::Integer, 0, 121 } }, //TODO - could make a lookup of CC controller names? 121 is from the manual
		{ 18, { "MIDI Pedal 2 Controller", "MIDI", Value(), ValueType::Integer, 0, 121 } },
		{ 19, { "MIDI Pedal 3 Controller", "MIDI", Value(), ValueType::Integer, 0, 121 } },
		{ 20, { "MIDI Pedal 4 Controller", "MIDI", Value(), ValueType::Integer, 0, 121 } },
		{ 32, { "MIDI Echo Enable", "MIDI", Value(), ValueType::Bool, 0, 1 } },
		{ 35, { "MIDI Mono Mode (Guitar)", "MIDI", Value(), ValueType::Integer, 0, 9 } },
		{ 165, { "Bank Lock Enable", "MIDI", Value(), ValueType::Bool, 0, 1 } }, // (In MSB only)
		{ 4, { "Vibrato Waveform", "Global Vibrato", Value(), ValueType::Lookup, 0, 7, { {0, "Triangle" }, { 1, "Saw up" }, { 2, "Saw Down" }, { 3, "Square" }, { 4, "Random" }, { 5, "Noise" } } } },
		{ 1, { "Vibrato Speed", "Global Vibrato", Value(), ValueType::Integer, 0, 63 } },
		{ 5, { "Vibrato Amplitude", "Global Vibrato", Value(), ValueType::Integer, 0, 63 } },		
		{ 2, { "Vibrato Speed Mod Source", "Global Vibrato", Value(), ValueType::Lookup, 0, 2, { {0, "Off" }, { 1, "Lever 2" }, { 2, "Pedal 1" } } } },
		{ 3, { "Vibrato Speed Mod Amount", "Global Vibrato", Value(), ValueType::Integer, 0, 63 } },
		{ 6, { "Vibrato Amp Mod Source", "Global Vibrato", Value(), ValueType::Lookup, 0, 2, { {0, "Off" }, { 1, "Lever 2" }, { 2, "Pedal 1" } } } },
		{ 7, { "Vibrato Amp Mod Amount", "Global Vibrato", Value(), ValueType::Integer, 0, 63 } },
		{ 164, { "Bend Range", "Controls", Value(), ValueType::Integer, 1, 24 } },		
		{ 166, { "Number of Units", "Group Mode", Value(), ValueType::Integer, 1, 6 } }, 
		{ 167, { "Current Unit Number", "Group Mode", Value(), ValueType::Integer, 0, 7 } }, // (In MSB only)
		{ 168, { "Group Mode Enable", "Group Mode", Value(), ValueType::Bool, 0, 1 } }, // (In MSB only)
		{ 169, { "Unison Enable", "General", Value(), ValueType::Bool, 0, 1 } }, 
		{ 170, { "Volume Invert Enable", "General", Value(), ValueType::Bool, 0, 1 } }, 
		{ 171, { "Memory Protect Enable", "General", Value(), ValueType::Bool, 0, 1 } }, 
	};

	juce::MidiMessage Matrix1000::requestEditBufferDump()
	{
		return createRequest(EDIT_BUFFER, 0x00);
	}

	MidiMessage Matrix1000::createRequest(REQUEST_TYPE typeNo, uint8 number) const
	{
		/* 04H - Request Data

		F0H 10H 06H 04H <type> <number> F7H

		<type>     = 0 to request all patches in current bank and master parameters.
				   = 1 to request a single patch from the current bank
				   = 3 to request master parameters
				   = 4 to request edit buffer
		<number>   = 0 when <type> = 0 or 3
				   = Number of patch requested when <type> = 1
				   */
		return MidiHelpers::sysexMessage({ OBERHEIM, MATRIX6_1000, REQUEST_DATA, (uint8)typeNo, (uint8)((typeNo == REQUEST_TYPE::SINGLE_PATCH) ? number : 0) });
	}

	juce::MidiMessage Matrix1000::createBankSelect(MidiBankNumber bankNo) const {
		if (!bankNo.isValid()) {
			jassert(false);
			return MidiMessage();
		}
		return MidiHelpers::sysexMessage({ OBERHEIM, MATRIX6_1000, SET_BANK, (uint8)bankNo.toZeroBased() });
	}

	MidiMessage Matrix1000::createBankUnlock() const {
		return MidiHelpers::sysexMessage({ OBERHEIM, MATRIX6_1000, BANK_UNLOCK });
	}

	void Matrix1000::initGlobalSettings()
	{
		// Loop over it and fill out the GlobalSettings Properties
		globalSettings_.clear();
		for (size_t i = 0; i < kMatrix1000GlobalSettings.size(); i++) {
			auto setting = std::make_shared<TypedNamedValue>(kMatrix1000GlobalSettings[i].typedNamedValue);
			globalSettings_.push_back(setting);
			setting->value.addListener(this);
		}
	}

	void Matrix1000::valueChanged(Value& value)
	{
		//TODO to be implemented
		ignoreUnused(value);
	}

	bool Matrix1000::isOwnSysex(MidiMessage const &message) const
	{
		return message.isSysEx()
			&& message.getSysExDataSize() > 1
			&& message.getSysExData()[0] == OBERHEIM
			&& message.getSysExData()[1] == MATRIX6_1000;
	}

	int Matrix1000::numberOfBanks() const
	{
		return 10;
	}

	int Matrix1000::numberOfPatches() const
	{
		return 100;
	}

	std::string Matrix1000::friendlyBankName(MidiBankNumber bankNo) const
	{
		return (boost::format("%03d - %03d") % (bankNo.toZeroBased() * numberOfPatches()) % (bankNo.toOneBased() * numberOfPatches() - 1)).str();
	}

	bool Matrix1000::isEditBufferDump(const MidiMessage& message) const
	{
		return isOwnSysex(message)
			&& message.getSysExDataSize() > 3
			&& message.getSysExData()[2] == SINGLE_PATCH_DATA
			&& message.getSysExData()[3] == 0x00; // Unspecified, but let's assume else it's a single program dump
	}


	bool Matrix1000::isSingleProgramDump(const MidiMessage& message) const
	{
		return isOwnSysex(message)
			&& message.getSysExDataSize() > 3
			&& message.getSysExData()[2] == SINGLE_PATCH_DATA
			&& message.getSysExData()[3] >= 0x00
			&& message.getSysExData()[3] < 100; // Should be a valid program number in this bank
	}

	std::shared_ptr<Patch> Matrix1000::patchFromProgramDumpSysex(const MidiMessage& message) const
	{
		if (isSingleProgramDump(message)) {
			auto matrixPatch = std::make_shared<Matrix1000Patch>(unescapeSysex(&message.getSysExData()[4], message.getSysExDataSize() - 4));
			matrixPatch->setPatchNumber(MidiProgramNumber::fromZeroBase(message.getSysExData()[3]));
			return matrixPatch;
		}
		return nullptr;
	}

	std::vector<juce::MidiMessage> Matrix1000::patchToProgramDumpSysex(const Patch &patch) const
	{
		//TODO - this is nearly a copy of the patchToSysex
		uint8 programNo = patch.patchNumber()->midiProgramNumber().toZeroBased() % 100;
		std::vector<uint8> singleProgramDump({ OBERHEIM, MATRIX6_1000, SINGLE_PATCH_DATA, programNo });
		auto patchdata = escapeSysex(patch.data());
		std::copy(patchdata.begin(), patchdata.end(), std::back_inserter(singleProgramDump));
		return std::vector<MidiMessage>({ MidiHelpers::sysexMessage(singleProgramDump) });
	}

	std::vector<MidiMessage>  Matrix1000::requestBankDump(MidiBankNumber bankNo) const
	{
		if (!bankNo.isValid()) {
			return {};
		}
		return { createBankSelect(bankNo), createRequest(BANK_AND_MASTER, 0) };
	}

	bool Matrix1000::isBankDump(const MidiMessage& message) const
	{
		// This is more "is part of bank dump"
		return isSingleProgramDump(message);
	}

	bool Matrix1000::isBankDumpFinished(std::vector<MidiMessage> const &bankDump) const
	{
		// Count the number of patch dumps in that stream
		int found = 0;
		for (auto message : bankDump) {
			if (isSingleProgramDump(message)) found++;
		}
		return found == numberOfPatches();
	}

	TPatchVector Matrix1000::patchesFromSysexBank(const MidiMessage& message) const
	{
		ignoreUnused(message);
		// Coming here would be a logic error - the Matrix has a patch dump request, but the synth will reply with lots of individual patch dumps
		throw std::logic_error("The method or operation is not implemented.");
	}

	juce::MidiMessage Matrix1000::saveEditBufferToProgram(int programNumber)
	{
		jassert(programNumber >= 0 && programNumber < 1000);
		uint8 bank = (uint8)(programNumber / 100);
		uint8 num = (uint8)(programNumber % 100);
		return MidiHelpers::sysexMessage({ OBERHEIM, MATRIX6_1000, STORE_EDIT_BUFFER, num, bank, 0 /* this says group mode off */ });
	}

	std::vector<juce::MidiMessage> Matrix1000::requestPatch(int programNumber)
	{
		std::vector<MidiMessage> result;
		jassert(programNumber >= 0 && programNumber < 1000);
		uint8 bank = (uint8)(programNumber / 100);
		//uint8 num = (uint8)(programNumber % 100);
		result.push_back(createBankSelect(MidiBankNumber::fromZeroBase(bank)));
		result.push_back(createBankUnlock());
		result.push_back(createRequest(SINGLE_PATCH, (uint8) programNumber));
		return result;
	}

	std::shared_ptr<Patch> Matrix1000::patchFromSysex(const MidiMessage& message) const
	{
		if (!isEditBufferDump(message)) {
			jassert(false);
			return std::make_shared<Matrix1000Patch>(PatchData());
		}

		// Patch number - currently unused, I think. The problem is that you don't know which bank this program belongs to, because
		// it depends on how this was requested.
		//uint8 patchNumber = message.getSysExData()[3];

		// Decode the data
		const uint8 *startOfData = &message.getSysExData()[4];
		auto matrixPatch = std::make_shared<Matrix1000Patch>(unescapeSysex(startOfData, message.getSysExDataSize() - 4));
		return matrixPatch;
	}

	std::shared_ptr<DataFile> Matrix1000::patchFromPatchData(const Synth::PatchData &data, std::string const &name, MidiProgramNumber place) const {
		ignoreUnused(name);
		auto newPatch = std::make_shared<Matrix1000Patch>(data);
		newPatch->setPatchNumber(place);
		return newPatch;
	}


	Synth::PatchData Matrix1000::filterVoiceRelevantData(std::shared_ptr<DataFile> unfilteredData) const
	{
		// The first 8 bytes are the name in the sysex data, and will be cleared out by the Matrix 1000 if you send a patch to it and then back again
		// So we must ignore them by setting them to 0!
		return Patch::blankOut(kMatrix1000BlankOutZones, unfilteredData->data());
	}

	/*std::string Matrix1000::patchToText(PatchData const &patch) {
		SynthSetup sound = patchToSynthSetup(patch);
		std::string result = sound.toText();
		return result;
	}

	SynthSetup Matrix1000::patchToSynthSetup(PatchData const &patch)
	{
		Matrix1000Patch m1000(patch);
		SynthSetup sound(m1000.patchName());

		// Read the oscillator model from the patch data
		std::string shape1, shape2;
		if (m1000.param(DCO_1_Waveform_Enable_Pulse)) {
			if (m1000.param(DCO_1_Initial_Pulse_width) == 31)
				shape1 = "Square";
			else
				shape1 = "Pulse";
		}
		if (m1000.param(DCO_1_Waveform_Enable_Saw)) {
			auto wave = m1000.param(DCO_1_Initial_Waveshape_0);
			if (wave == 0)
				shape2 = "Saw";
			else if (wave == 31)
				shape2 = "Triangle";
			else
				shape2 = "Sawwave";
		}
		auto osc1 = std::make_shared<CompoundOsc>("Osc1", m1000.param(DCO_1_Initial_Frequency_LSB), shape1, m1000.param(DCO_1_Initial_Pulse_width), shape2, m1000.param(DCO_1_Initial_Waveshape_0));
		if (m1000.param(DCO_1_Click) != 0) {
			osc1->addSpecial(std::make_shared<FlagParameter>("click"));
		}

		if (m1000.param(DCO_2_Waveform_Enable_Pulse)) {
			if (m1000.param(DCO_2_Initial_Pulse_width) == 31)
				shape1 = "Square";
			else
				shape1 = "Pulse";
		}
		if (m1000.param(DCO_2_Waveform_Enable_Saw)) {
			auto wave = m1000.param(DCO_2_Initial_Waveshape_0);
			if (wave == 0)
				shape2 = "Saw";
			else if (wave == 31)
				shape2 = "Triangle";
			else
				shape2 = "Sawwave";
		}
		sound.oscillators_.push_back(osc1);

		auto osc2 = std::make_shared<CompoundOsc>("Osc2", m1000.param(DCO_2_Initial_Frequency_LSB), shape1, m1000.param(DCO_2_Initial_Pulse_width), shape2, m1000.param(DCO_2_Initial_Waveshape_0));
		if (m1000.param(DCO_2_Waveform_Enable_Noise) != 0) {
			osc2->addSpecial(std::make_shared<SoundGeneratingFlag>("Noise"));
		}
		if (m1000.param(DCO_2_Click) != 0) {
			osc2->addSpecial(std::make_shared<FlagParameter>("Click"));
		}
		if (m1000.param(DCO_2_Detune) != 0) {
			osc2->addSpecial(std::make_shared<ValueParameter>("Detune", m1000.param(DCO_2_Detune)));
		}
		sound.oscillators_.push_back(osc2);
		sound.mix_ = m1000.param(MIX);

		// Information on the filter - the Matrix only has a 4-pole filter, so we can hardcode that
		auto filter = std::make_shared<Filter>("LPF",
			Frequency::knobPercentage(m1000.param(VCF_Initial_Frequency_LSB) / 63.0f),
			4, m1000.param(VCF_Initial_Resonance), m1000.param(VCF_FM_Initial_Amount));
		sound.filters_.push_back(filter);

		// Time to create the envelopes used!
		auto env1 = std::make_shared<Envelope>("Env 1",
			TimeValue::midiValue(m1000.param(Env_1_Initial_Delay_Time), 63),
			TimeValue::midiValue(m1000.param(Env_1_Initial_Attack_Time), 63),
			TimeValue::midiValue(m1000.param(Env_1_Initial_Decay_Time), 63),
			ControlValue::midiValue(m1000.param(Env_1_Sustain_Level), 63),
			TimeValue::midiValue(m1000.param(Env_1_Initial_Release_Time), 63));
		sound.envelopes_.push_back(env1);
		auto env2 = std::make_shared<Envelope>("Env 2",
			TimeValue::midiValue(m1000.param(Env_2_Initial_Delay_Time), 63),
			TimeValue::midiValue(m1000.param(Env_2_Initial_Attack_Time), 63),
			TimeValue::midiValue(m1000.param(Env_2_Initial_Decay_Time), 63),
			ControlValue::midiValue(m1000.param(Env_2_Sustain_Level), 63),
			TimeValue::midiValue(m1000.param(Env_2_Initial_Release_Time), 63));
		sound.envelopes_.push_back(env2);
		auto env3 = std::make_shared<Envelope>("Env 3",
			TimeValue::midiValue(m1000.param(Env_3_Initial_Delay_Time), 63),
			TimeValue::midiValue(m1000.param(Env_3_Initial_Attack_Time), 63),
			TimeValue::midiValue(m1000.param(Env_3_Initial_Decay_Time), 63),
			ControlValue::midiValue(m1000.param(Env_3_Sustain_Level), 63),
			TimeValue::midiValue(m1000.param(Env_3_Initial_Release_Time), 63));
		sound.envelopes_.push_back(env3);

		// Now to the LFOs
		auto lfo1 = std::make_shared<LFO>("LFO 1", m1000.lookupValue(LFO_1_Waveshape), (float)m1000.param(LFO_1_Initial_Speed), (float)m1000.param(LFO_1_Initial_Amplitude));
		auto lfo2 = std::make_shared<LFO>("LFO 2", m1000.lookupValue(LFO_2_Waveshape), (float)m1000.param(LFO_2_Initial_Speed), (float)m1000.param(LFO_2_Initial_Amplitude));
		sound.lfos_.push_back(lfo1);
		sound.lfos_.push_back(lfo2);

		// Now, construct text to describe the fixed modulations, sorted by modulate source
		std::vector<Modulation> modulations;

		// First, collect modulations which are distributed quite through the normal parameters
		if (m1000.paramActive(DCO_1_Fixed_Modulations_PitchBend)) {
			modulations.push_back(Modulation("Pitch Bend", "DCO 1 freq", 63));
		}
		if (m1000.paramActive(DCO_1_Fixed_Modulations_Vibrato)) {
			modulations.push_back(Modulation("Vibrato", "DCO 1 freq", 63));
		}
		if (m1000.paramActive(DCO_2_Fixed_Modulations_PitchBend)) {
			modulations.push_back(Modulation("Pitch Bend", "DCO 2 freq", 63));
		}
		if (m1000.paramActive(DCO_2_Fixed_Modulations_Vibrato)) {
			modulations.push_back(Modulation("Vibrato", "DCO 2 freq", 63));
		}
		if (m1000.paramActive(DCO_1_Fixed_Modulations_Portamento)) {
			modulations.push_back(Modulation("Portamento", "DCO 1 freq", 63));
		}
		if (m1000.paramActive(DCO_2_Fixed_Modulations_Portamento)) {
			modulations.push_back(Modulation("Portamento", "DCO 2 freq", 63));
		}
		if (!m1000.paramActive(DCO_2_Fixed_Modulations_KeyboardTracking)) {
			modulations.push_back(Modulation("KeyboardTracking off", "DCO 2 freq", 63));
		}
		if (m1000.paramActive(VCF_Fixed_Modulations_Lever1)) {
			modulations.push_back(Modulation("Pitch Bend", "Cutoff", 63));
		}
		if (m1000.paramActive(VCF_Fixed_Modulations_Vibrato)) {
			modulations.push_back(Modulation("Vibrato", "Cutoff", 63));
		}
		if (m1000.paramActive(VCF_Keyboard_Modulation_Portamento)) {
			modulations.push_back(Modulation("Portamento", "Cutoff", 63));
		}
		if (m1000.paramActive(VCF_Keyboard_Modulation_Key)) {
			modulations.push_back(Modulation("Key", "Cutoff", 63));
		}
		if (m1000.param(Tracking_Generator_Input_Source_Code) != 0) {
			modulations.push_back(Modulation(m1000.lookupValue(Tracking_Generator_Input_Source_Code), "Tracking Generator", m1000.param(Tracking_Generator_Input_Source_Code)));
		}

		// The Matrix 10000 has 18 dedicated fixed modulations, kind of the "most commonly used" modulations stored in these flags
		if (m1000.paramActive(DCO_1_Freq_by_LFO_1_Amount)) {
			modulations.push_back(Modulation("LFO 1", "OSC 1 pitch", m1000.param(DCO_1_Freq_by_LFO_1_Amount)));
		}
		if (m1000.paramActive(DCO_1_PW_by_LFO_2_Amount)) {
			modulations.push_back(Modulation("LFO 2", "OSC 1 pulse width", m1000.param(DCO_1_PW_by_LFO_2_Amount)));
		}
		if (m1000.paramActive(DCO_2_Freq_by_LFO_1_Amount)) {
			modulations.push_back(Modulation("LFO 1", "OSC 2 pitch", m1000.param(DCO_2_Freq_by_LFO_1_Amount)));
		}
		if (m1000.paramActive(DCO_2_PW_by_LFO_2_Amount)) {
			modulations.push_back(Modulation("LFO 2", "OSC 2 pulse width", m1000.param(DCO_2_PW_by_LFO_2_Amount)));
		}
		if (m1000.paramActive(VCF_Freq_by_Env_1_Amount)) {
			modulations.push_back(Modulation("Env 1", "Cutoff", m1000.param(VCF_Freq_by_Env_1_Amount)));
		}
		if (m1000.paramActive(VCF_Freq_by_Pressure_Amount)) {
			modulations.push_back(Modulation("Aftertouch", "Cutoff", m1000.param(VCF_Freq_by_Pressure_Amount)));
		}
		if (m1000.paramActive(VCA_1_by_Velocity_Amount)) {
			modulations.push_back(Modulation("Velocity", "Amplitude 1", m1000.param(VCA_1_by_Velocity_Amount)));
		}
		if (m1000.paramActive(VCA_2_by_Env_2_Amount)) {
			modulations.push_back(Modulation("Env 2", "Amplitude 2", m1000.param(VCA_2_by_Env_2_Amount)));
		}
		if (m1000.paramActive(Env_1_Amplitude_by_Velocity_Amount)) {
			modulations.push_back(Modulation("Velocity", "Env 1 Amplitude", m1000.param(Env_1_Amplitude_by_Velocity_Amount)));
		}
		if (m1000.paramActive(Env_2_Amplitude_by_Velocity_Amount)) {
			modulations.push_back(Modulation("Velocity", "Env 2 Amplitude", m1000.param(Env_2_Amplitude_by_Velocity_Amount)));
		}
		if (m1000.paramActive(Env_3_Amplitude_by_Velocity_Amount)) {
			modulations.push_back(Modulation("Velocity", "Env 3 Amplitude", m1000.param(Env_3_Amplitude_by_Velocity_Amount)));
		}
		if (m1000.paramActive(LFO_1_Amp_by_Ramp_1_Amount)) {
			modulations.push_back(Modulation("Ramp 1", "LFO 1 Amplitude", m1000.param(LFO_1_Amp_by_Ramp_1_Amount)));
		}
		if (m1000.paramActive(LFO_2_Amp_by_Ramp_2_Amount)) {
			modulations.push_back(Modulation("Ramp 2", "LFO 2 Amplitude", m1000.param(LFO_2_Amp_by_Ramp_2_Amount)));
		}
		if (m1000.paramActive(Portamento_rate_by_Velocity_Amount)) {
			modulations.push_back(Modulation("Velocity", "Portamento Rate", m1000.param(Portamento_rate_by_Velocity_Amount)));
		}
		if (m1000.paramActive(VCF_FM_Amount_by_Env_3_Amount)) {
			modulations.push_back(Modulation("Env 3", "FM Amount", m1000.param(VCF_FM_Amount_by_Env_3_Amount)));
		}
		if (m1000.paramActive(VCF_FM_Amount_by_Pressure_Amount)) {
			modulations.push_back(Modulation("Aftertouch", "FM Amount", m1000.param(VCF_FM_Amount_by_Pressure_Amount)));
		}
		if (m1000.paramActive(LFO_1_Speed_by_Pressure_Amount)) {
			modulations.push_back(Modulation("Aftertouch", "LFO 1 freq", m1000.param(LFO_1_Speed_by_Pressure_Amount)));
		}
		if (m1000.paramActive(LFO_2_Speed_by_Keyboard_Amount)) {
			modulations.push_back(Modulation("Key", "LFO 2 freq", m1000.param(LFO_2_Speed_by_Keyboard_Amount)));
		}

		// But then, it also has 10 slots for a proper modulation matrix!
		if (m1000.paramActive(Matrix_Modulation_Bus_0_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_0_Source_Code), m1000.lookupValue(MM_Bus_0_Destination_Code), m1000.param(M_Bus_0_Amount)));
		}
		if (m1000.paramActive(Matrix_Modulation_Bus_1_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_1_Source_Code), m1000.lookupValue(MM_Bus_1_Destination_Code), m1000.param(M_Bus_1_Amount)));
		}
		if (m1000.paramActive(Matrix_Modulation_Bus_2_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_2_Source_Code), m1000.lookupValue(MM_Bus_2_Destination_Code), m1000.param(M_Bus_2_Amount)));
		}
		if (m1000.paramActive(Matrix_Modulation_Bus_3_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_3_Source_Code), m1000.lookupValue(MM_Bus_3_Destination_Code), m1000.param(M_Bus_3_Amount)));
		}
		if (m1000.paramActive(Matrix_Modulation_Bus_4_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_4_Source_Code), m1000.lookupValue(MM_Bus_4_Destination_Code), m1000.param(M_Bus_4_Amount)));
		}
		if (m1000.paramActive(Matrix_Modulation_Bus_5_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_5_Source_Code), m1000.lookupValue(MM_Bus_5_Destination_Code), m1000.param(M_Bus_5_Amount)));
		}
		if (m1000.paramActive(Matrix_Modulation_Bus_6_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_6_Source_Code), m1000.lookupValue(MM_Bus_6_Destination_Code), m1000.param(M_Bus_6_Amount)));
		}
		if (m1000.paramActive(Matrix_Modulation_Bus_7_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_7_Source_Code), m1000.lookupValue(MM_Bus_7_Destination_Code), m1000.param(M_Bus_7_Amount)));
		}
		if (m1000.paramActive(Matrix_Modulation_Bus_8_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_8_Source_Code), m1000.lookupValue(MM_Bus_8_Destination_Code), m1000.param(M_Bus_8_Amount)));
		}
		if (m1000.paramActive(Matrix_Modulation_Bus_9_Source_Code)) {
			modulations.push_back(Modulation(m1000.lookupValue(Matrix_Modulation_Bus_9_Source_Code), m1000.lookupValue(MM_Bus_9_Destination_Code), m1000.param(M_Bus_9_Amount)));
		}

		// Copy modulations into sound
		for (auto modulation : modulations) {
			sound.modulations_.push_back(std::make_shared<Modulation>(modulation));
		}

		return sound;
	}

	std::shared_ptr<Patch> Matrix1000::synthSetupToPatch(SynthSetup const &sound, std::function<void(std::string warning)> logWarning)
	{
		throw std::logic_error("The method or operation is not implemented.");
	}*/

	bool Matrix1000::canChangeInputChannel() const
	{
		//TODO - actually you can do that, but it requires a complete roundtrip to query the global page, change the channel, and send the changed page back.
		return false;
	}

	void Matrix1000::changeInputChannel(MidiController *controller, MidiChannel channel, std::function<void()> onFinished)
	{
		ignoreUnused(controller, channel, onFinished);
		throw new std::runtime_error("Illegal state");
	}

	MidiChannel Matrix1000::getInputChannel() const
	{
		return channel();
	}

	bool Matrix1000::hasMidiControl() const
	{
		return false;
	}

	bool Matrix1000::isMidiControlOn() const
	{
		return true;
	}

	void Matrix1000::setMidiControl(MidiController *controller, bool isOn)
	{
		ignoreUnused(controller, isOn);
		throw new std::runtime_error("Illegal state");
	}

	/*std::vector<std::string> Matrix1000::presetNames()
	{
		auto basename = (boost::format("Knobkraft M1000 %d") % channel().toOneBasedInt()).str();
		return { basename + " Main", basename + " Second" };
	}*/

	juce::MidiMessage Matrix1000::deviceDetect(int channel)
	{
		std::vector<uint8> sysEx({ 0x7e, (uint8)channel, 0x06, 0x01 });
		return MidiMessage::createSysExMessage(&sysEx[0], (int)sysEx.size());
	}

	int Matrix1000::deviceDetectSleepMS()
	{
		// The Matrix1000 can be sluggish to react on a Device ID request, better wait for 200 ms
		return 200;
	}

	MidiChannel Matrix1000::channelIfValidDeviceResponse(const MidiMessage &message)
	{
		auto data = message.getSysExData();
		if (message.getSysExDataSize() < 3)
			return MidiChannel::invalidChannel();

		if (message.getSysExDataSize() == 13 &&
			data[0] == 0x7e &&
			data[2] == 0x06 &&
			data[3] == 0x02 &&
			data[4] == OBERHEIM &&
			data[5] == MATRIX6_1000 &&
			data[6] == 0x00 &&
			//data[7] == 0x02 && // Fam member = Matrix 1000
			data[8] == 0x00) {
			return MidiChannel::fromZeroBase(data[1]); // Return the channel reported by the Matrix
		}
		// Characters 9 to 12 specify the firmware revision
		return MidiChannel::invalidChannel();
	}

	bool Matrix1000::needsChannelSpecificDetection()
	{
		return true;
	}

	void Matrix1000::setGlobalSettingsFromDataFile(std::shared_ptr<DataFile> dataFile)
	{
		auto settingsArray = unescapeSysex(dataFile->data().data(), (int) dataFile->data().size());
		if (settingsArray.size() == 172) {
			for (size_t i = 0; i < kMatrix1000GlobalSettings.size(); i++) {
				if (i < settingsArray.size()) {
					int intValue = settingsArray[kMatrix1000GlobalSettings[i].sysexIndex] + kMatrix1000GlobalSettings[i].displayOffset;
					if (kMatrix1000GlobalSettings[i].isTwosComplement) {
						// Very special code that only works because there are just two fields in the global settings that need it
						// Master transpose and Master tuning
						if (intValue > 127) {
							intValue = (int8) intValue;
						}
					}
					globalSettings_[i]->value.setValue(var(intValue));
				}
			}
		}
		else {
			SimpleLogger::instance()->postMessage("Ignoring Matrix1000 global settings data - unescaped block size is not 172 bytes");
			jassert(settingsArray.size() == 172);
		}
	}

	std::vector<std::shared_ptr<TypedNamedValue>> Matrix1000::getGlobalSettings()
	{
		return globalSettings_;
	}

	midikraft::DataFileLoadCapability * Matrix1000::loader()
	{
		return globalSettingsLoader_;
	}

	int Matrix1000::settingsDataFileType() const
	{
		return DF_MATRIX1000_SETTINGS;
	}

	std::vector<juce::MidiMessage> Matrix1000::patchToSysex(const Patch &patch) const
	{
		std::vector<uint8> editBufferDump({ OBERHEIM, MATRIX6_1000, SINGLE_PATCH_TO_EDIT_BUFFER, 0x00 });
		auto patchdata = escapeSysex(patch.data());
		std::copy(patchdata.begin(), patchdata.end(), std::back_inserter(editBufferDump));
		return std::vector<MidiMessage>({ MidiHelpers::sysexMessage(editBufferDump) });
	}

	Matrix1000::Matrix1000()
	{
		globalSettingsLoader_ = new Matrix1000_GlobalSettings_Loader(this);
		initGlobalSettings();
	}

	Matrix1000::~Matrix1000()
	{
		delete globalSettingsLoader_;
	}

	std::string Matrix1000::getName() const
	{
		return "Oberheim Matrix 1000";
	}

	Synth::PatchData Matrix1000::unescapeSysex(const uint8 *sysExData, int sysExLen) const
	{
		Synth::PatchData result;

		// The Matrix 1000 does two things: Calculate a checksum (yes, it's a sum) and pack each byte into two nibbles. That's not really
		// data efficient, but hey, a 2 MHz 8-bit CPU must be able to pack and unpack that at MIDI speed!
		uint8 checksum = 0;
		for (int index = 0; index < sysExLen; ) {
			// Read one more data byte from two bytes
			uint8 byte = sysExData[index] | sysExData[index + 1] << 4;
			result.push_back(byte);
			checksum += byte;
			index += 2;
			if (index == sysExLen - 1) {
				// This is the checksum!
				if (sysExData[index] != (checksum & 0x7f)) {
					// Invalid checksum, don't use this
					result.clear();
					return result;
				}
				index++;
			}
		}
		return result;
	}

	std::vector<juce::uint8> Matrix1000::escapeSysex(const PatchData &programEditBuffer) const
	{
		std::vector<uint8> result;
		// We generate the nibbles and the checksum
		int checksum = 0;
		for (int i = 0; i < programEditBuffer.size(); i++) {
			checksum += programEditBuffer[i];
			result.push_back(programEditBuffer[i] & 0x0f);
			result.push_back((programEditBuffer[i] & 0xf0) >> 4);
		}
		result.push_back(checksum & 0x7f);
		return result;
	}

	/*void Matrix1000::setupBCR2000(MidiController *controller, BCR2000 &bcr, SimpleLogger *logger) {
		if (bcr.channel().isValid() && channel().isValid()) {
			// Make sure to bake the current channel of the Matrix into the setup for the BCR
			Matrix1000BCR sysex_bcr(channel().toZeroBasedInt());
			Matrix1000BCR nrpn_bcr(channel().toZeroBasedInt(), false);

			// Now, manually build both again with only one header and footer
			std::string bothPresets;
			bothPresets = BCR2000::generateBCRHeader();
			bothPresets += sysex_bcr.generateBCR(presetNames()[0], presetNames()[1], BCR2000_Preset_Positions::MATRIX1000_P1, false);
			//bothPresets += nrpn_bcr.generateBCR(3, false);
			bothPresets += BCR2000::generateBCREnd(BCR2000_Preset_Positions::MATRIX1000_P1);
			controller->enableMidiInput(bcr.midiInput());
			bcr.sendSysExToBCR(controller->getMidiOutput(bcr.midiOutput()), bcr.convertToSyx(bothPresets), *controller, logger);
		}
	}

	void Matrix1000::syncDumpToBCR(MidiProgramNumber programNumber, MidiController *controller, BCR2000 &bcr, SimpleLogger *logger) {
		if (matrixBCRSyncHandler_.is_nil()) {
			matrixBCRSyncHandler_ = EditBufferHandler::makeOne();
			controller->setNextEditBufferHandler(matrixBCRSyncHandler_, [this, logger, controller, &bcr](MidiMessage const &message) {
				if (isEditBufferDump(message)) {
					logger->postMessage("Got Edit Buffer from Matrix and now updating the BCR2000");
					auto patch = std::dynamic_pointer_cast<Matrix1000Patch>(patchFromSysex(message));

					// Loop over all parameters in patch and send out the appropriate NRPNs, if they are within the BCR2000 definition...
					std::vector<MidiMessage> updates;
					for (auto paramDef : patch->allParameterDefinitions()) {
						auto m1000p = dynamic_cast<Matrix1000ParamDefinition *>(paramDef);
						if (m1000p != nullptr) {
							if (Matrix1000BCR::parameterHasControlAssigned(m1000p->id()) && !(m1000p->bitposition() != -1)) {
								// Remember to not create NRPN messages for sub-bit-parameters - else we would create multiple messages for the same sysex byte
								auto nrpn = Matrix1000BCR::createNRPNforParam(dynamic_cast<Matrix1000ParamDefinition *>(paramDef), *patch, channel().toZeroBasedInt());
								std::copy(nrpn.begin(), nrpn.end(), std::back_inserter(updates));
							}
						}
					}
					MidiBuffer updateBuffer = MidiHelpers::bufferFromMessages(updates);
					// Send this to BCR
					controller->getMidiOutput(bcr.midiOutput())->sendBlockOfMessagesNow(updateBuffer);
				}
			});
		}
		// We want to talk to the Matrix now
		controller->enableMidiInput(midiInput());
		controller->getMidiOutput(midiOutput())->sendMessageNow(requestEditBufferDump());
	}

	void Matrix1000::setupBCR2000View(BCR2000_Component &view) {
		//TODO
	}

	void Matrix1000::setupBCR2000Values(BCR2000_Component &view, Patch *patch)
	{
		//TODO
	}
	*/
}