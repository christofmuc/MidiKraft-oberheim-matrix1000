/*
   Copyright (c) 2020 Christof Ruch. All rights reserved.

   Dual licensed: Distributed under Affero GPL license by default, an MIT license is available for purchase
*/

#pragma once

#include "JuceHeader.h"

#include "DataFileLoadCapability.h"

#include "Matrix1000.h"

namespace midikraft {

	class Matrix1000_GlobalSettings_Loader : public SingleMessageDataFileLoadCapability {
	public:
		Matrix1000_GlobalSettings_Loader(Matrix1000 *matrix1000);

		virtual std::vector<MidiMessage> requestDataItem(int itemNo, DataStreamType dataTypeID) override;
		virtual bool isDataFile(const MidiMessage &message, DataFileType dataTypeID) const override;
		virtual std::vector<std::shared_ptr<DataFile>> loadData(std::vector<MidiMessage> messages, DataStreamType dataTypeID) const override;
		virtual bool isPartOfDataFileStream(const MidiMessage &message, DataStreamType dataTypeID) const override;
		virtual std::vector<DataFileDescription> dataTypeNames() const  override;
		virtual std::vector<DataFileImportDescription> dataFileImportChoices() const override;

	private:
		Matrix1000 *matrix1000_;
	};

}

