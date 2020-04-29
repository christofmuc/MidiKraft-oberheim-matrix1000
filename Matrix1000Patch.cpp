/*
   Copyright (c) 2020 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#include "Matrix1000Patch.h"

#include "Matrix1000ParamDefinition.h"

#include <boost/format.hpp>
#include <unicode/ucnv.h>

#include <regex>

namespace midikraft {

	const int kMatrix1000DataType = 0; // The Matrix1000 has just one data file type - the patch. No layers, voices, alternate tunings...

	std::string Matrix1000PatchNumber::friendlyName() const {
		// The Matrix does a 3 digit display, with the first patch being "000" and the highest being "999"s 
		return (boost::format("%03d") % programNumber_.toZeroBased()).str();
	}

	Matrix1000Patch::Matrix1000Patch(Synth::PatchData const &patchdata) : Patch(kMatrix1000DataType, patchdata)
	{
	}

	std::string Matrix1000Patch::name() const
	{
		// The patch name are the first 8 bytes ASCII
		std::string name;
		for (int i = 0; i < 8; i++) {
			int charValue = at(i);
			if (charValue < 32) {
				//WTF? I found old factory banks that had the letters literally as the "Number of the letter in the Alphabet", 1-based
				charValue += 'A' - 1;
			}
			name.push_back((char)charValue);
		}
		return name;
	}

	void Matrix1000Patch::setName(std::string const &name)
	{
		// The String, coming from the UI, should be UTF8. We need some serious software to get back into ASCII land now
		// Let's use the ICU library
		char asciiResult[20];
		UErrorCode error = U_ZERO_ERROR;
		auto length = ucnv_convert("US-ASCII", "UTF-8", asciiResult, 20, name.c_str(), (int32_t) name.size(), &error);
		if (U_SUCCESS(error)) {
			for (int i = 0; i < 8; i++) {
				if (i < length) {
					if (asciiResult[i] == 0x1a) {
						// This is the substitution character ucnv_convert creates. We will replace it with something funny
						setAt(i, 0x40 /* @ */);
					}
					else if (asciiResult[i] > 0x5f) {
						// The sysex spec of the Matrix says it only uses 6 bits for the name, so I would think it does uppercase letters only (6 bits would be up to 2^6 = ascii 64)
						setAt(i, asciiResult[i] - 0x20); // This works because it would bring the highest ascii character 7f down to 5f
					}
					else if (asciiResult[i] < 0x20) {
						// Any other non-printable ASCII character, use a different substitution character, like "_"
						setAt(i, 0x5f);
					}
					else {
						// Valid ASCII
						setAt(i, asciiResult[i]);
					}
				}
				else {
					setAt(i, 0x20 /* space */);
				}
			}
		}
	}

	bool Matrix1000Patch::isDefaultName() const
	{
		std::regex matcher("BNK[0-9]: [0-9][0-9]", std::regex::icase);
		return std::regex_search(name(), matcher);
	}

	std::shared_ptr<PatchNumber> Matrix1000Patch::patchNumber() const {
		return std::make_shared<Matrix1000PatchNumber>(number_);
	}

	void Matrix1000Patch::setPatchNumber(MidiProgramNumber patchNumber) {
		number_ = Matrix1000PatchNumber(patchNumber);
	}

	int Matrix1000Patch::value(SynthParameterDefinition const &param) const
	{
		int result;
		auto intDefinition = dynamic_cast<SynthIntParameterCapability const *>(&param);
		if (intDefinition && intDefinition->valueInPatch(*this, result)) {
			return result;
		}
		throw new std::runtime_error("Invalid parameter");
	}

	int Matrix1000Patch::param(Matrix1000Param id) const
	{
		auto &param = Matrix1000ParamDefinition::param(id);
		return value(param);
	}

	SynthParameterDefinition const & Matrix1000Patch::paramBySysexIndex(int sysexIndex) const 
	{
		for (auto param : Matrix1000ParamDefinition::allDefinitions) {
			auto intParam = std::dynamic_pointer_cast<SynthIntParameterCapability>(param);
			if (intParam && intParam->sysexIndex() == sysexIndex) {
				//! TODO- this is a bad way to address the parameters, as this is not uniquely defined
				return *param;
			}
		}
		throw new std::runtime_error("Bogus call");
	}

	bool Matrix1000Patch::paramActive(Matrix1000Param id) const
	{
		auto &param = Matrix1000ParamDefinition::param(id);
		auto activeDefinition = dynamic_cast<SynthParameterActiveDetectionCapability const *>(&param);
		return !activeDefinition || activeDefinition->isActive(this);
	}

	std::string Matrix1000Patch::lookupValue(Matrix1000Param id) const
	{
		auto &param = Matrix1000ParamDefinition::param(id);
		return param.valueInPatchToText(*this);
	}

	std::vector<std::shared_ptr<SynthParameterDefinition>> Matrix1000Patch::allParameterDefinitions()
	{
		return Matrix1000ParamDefinition::allDefinitions;
	}

}
