// Copyright (c) 2012-2015, The CryptoNote developers, The Bytecoin developers
//
// This file is part of Bytecoin.
//
// Bytecoin is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Bytecoin is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with Bytecoin.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include "KeyOutput.h"
#include "MultisignatureOutput.h"
#include "UnsignedKeyInput.h"
#include "UnsignedMultisignatureInput.h"

namespace CryptoNote {

class UnsignedTransaction {
public:
  enum InputType {
    KEY_INPUT = 0,
    MULTISIGNATURE_INPUT = 1
  };

  enum OutputType {
    KEY_OUTPUT = 0,
    MULTISIGNATURE_OUTPUT = 1
  };

  struct KeyInputEntry {
    uint32_t index;
    UnsignedKeyInput input;
  };

  struct KeyOutputEntry {
    uint32_t index;
    KeyOutput output;
  };

  struct MultisignatureInputEntry {
    uint32_t index;
    UnsignedMultisignatureInput input;
  };

  struct MultisignatureOutputEntry {
    uint32_t index;
    MultisignatureOutput output;
  };

  UnsignedTransaction(
    uint64_t unlockTime,
    std::vector<KeyInputEntry>&& keyInputs,
    std::vector<MultisignatureInputEntry>&& multisignatureInputs,
    std::vector<KeyOutputEntry>&& keyOutputs,
    std::vector<MultisignatureOutputEntry>&& multisignatureOutputs,
    std::vector<uint8_t>&& extra);
  UnsignedTransaction(const UnsignedTransaction& other) = delete;
  UnsignedTransaction(UnsignedTransaction&& other);
  UnsignedTransaction& operator=(const UnsignedTransaction& other) = delete;
  uint64_t getUnlockTime() const;
  uint32_t getInputCount() const;
  InputType getInputType(uint32_t index) const;
  const UnsignedKeyInput& getKeyInput(uint32_t index) const;
  const UnsignedMultisignatureInput& getMultisignatureInput(uint32_t index) const;
  uint32_t getOutputCount() const;
  OutputType getOutputType(uint32_t index) const;
  const KeyOutput& getKeyOutput(uint32_t index) const;
  const MultisignatureOutput& getMultisignatureOutput(uint32_t index) const;
  const std::vector<uint8_t>& getExtra() const;

private:
  uint64_t unlockTime;
  std::vector<KeyInputEntry> keyInputs;
  std::vector<MultisignatureInputEntry> multisignatureInputs;
  std::vector<KeyOutputEntry> keyOutputs;
  std::vector<MultisignatureOutputEntry> multisignatureOutputs;
  std::vector<uint8_t> extra;
};

}
