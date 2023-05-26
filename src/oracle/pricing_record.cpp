// Copyright (c) 2019, Haven Protocol
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice, this list
//    of conditions and the following disclaimer in the documentation and/or other
//    materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors may be
//    used to endorse or promote products derived from this software without specific
//    prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
// THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Portions of this code based upon code Copyright (c) 2019, The Monero Project

#include "pricing_record.h"

#include "serialization/keyvalue_serialization.h"
#include "storages/portable_storage.h"

#include "string_tools.h"
namespace oracle
{

  namespace
  {
    struct pr_serialized
    {
      uint64_t zEPHUSD;
      uint64_t zEPHRSV;
      uint64_t timestamp;

      BEGIN_KV_SERIALIZE_MAP()
        KV_SERIALIZE(zEPHUSD)
        KV_SERIALIZE(zEPHRSV)
        KV_SERIALIZE(timestamp)
      END_KV_SERIALIZE_MAP()
    };
  }
  
  pricing_record::pricing_record() noexcept
    : zEPHUSD(0)
    , zEPHRSV(0)
    , timestamp(0) {}

  bool pricing_record::_load(epee::serialization::portable_storage& src, epee::serialization::section* hparent)
  {
    pr_serialized in{};
    if (in._load(src, hparent))
    {
      // Copy everything into the local instance
      zEPHUSD = in.zEPHUSD;
      zEPHRSV = in.zEPHRSV;
      timestamp = in.timestamp;
      return true;
    }

    // Report error here?
    return false;
  }

  bool pricing_record::store(epee::serialization::portable_storage& dest, epee::serialization::section* hparent) const
  {
    const pr_serialized out{zEPHUSD,zEPHRSV,timestamp};
    return out.store(dest, hparent);
  }

  pricing_record::pricing_record(const pricing_record& orig) noexcept
    : zEPHUSD(orig.zEPHUSD)
    , zEPHRSV(orig.zEPHRSV)
    , timestamp(orig.timestamp) {}

  pricing_record& pricing_record::operator=(const pricing_record& orig) noexcept
  {
    zEPHUSD = orig.zEPHUSD;
    zEPHRSV = orig.zEPHRSV;
    timestamp = orig.timestamp;
    return *this;
  }

  uint64_t pricing_record::operator[](const std::string& asset_type) const
  {
    if (asset_type == "ZEPH") {
      return zEPHUSD; // ZEPH spot price
    } else if (asset_type == "ZEPHUSD") {
      return COIN; // 1
    } else if (asset_type == "ZEPHRSV") {
      return zEPHRSV; // ZEPHRSV spot price
    } else {
     CHECK_AND_ASSERT_THROW_MES(false, "Asset type doesn't exist in pricing record!");
    }
  }
  
  bool pricing_record::equal(const pricing_record& other) const noexcept
  {
    return ((zEPHUSD == other.zEPHUSD) &&
      (zEPHRSV == other.zEPHRSV) &&
	    (timestamp == other.timestamp));
  }

  bool pricing_record::empty() const noexcept
  {
    const pricing_record empty_pr = oracle::pricing_record();
    return (*this).equal(empty_pr);
  }

  // overload for pr validation for block
  bool pricing_record::valid(cryptonote::network_type nettype, uint32_t hf_version, uint64_t bl_timestamp, uint64_t last_bl_timestamp) const 
  {
    if (hf_version < HF_VERSION_DJED) {
      if (!this->empty())
        return false;
    }

    if (this->empty())
        return true;
  
    // validate the timestmap
    if (this->timestamp > bl_timestamp + PRICING_RECORD_VALID_TIME_DIFF_FROM_BLOCK) {
      LOG_ERROR("Pricing record timestamp is too far in the future.");
      return false;
    }

    

    if (this->timestamp <= last_bl_timestamp - PRICING_RECORD_VALID_TIME_DIFF_FROM_BLOCK) {
      LOG_ERROR("Pricing record timestamp: " << this->timestamp << ", block timestamp: " << bl_timestamp);
      LOG_ERROR("Pricing record timestamp is too old.");
      return false;
    }

    return true;
  }
}
