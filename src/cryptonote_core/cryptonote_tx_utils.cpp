// Copyright (c) 2014-2023, The Monero Project
// 
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
// Parts of this file are originally copyright (c) 2012-2013 The Cryptonote developers

#include <unordered_set>
#include <random>
#include "include_base_utils.h"
#include "string_tools.h"
using namespace epee;

#include <boost/multiprecision/cpp_bin_float.hpp>
namespace multiprecision = boost::multiprecision;

#include "common/apply_permutation.h"
#include "cryptonote_tx_utils.h"
#include "cryptonote_config.h"
#include "blockchain.h"
#include "blockchain_db/blockchain_db.h"
#include "cryptonote_basic/miner.h"
#include "cryptonote_basic/tx_extra.h"
#include "crypto/crypto.h"
#include "crypto/hash.h"
#include "ringct/rctSigs.h"

using namespace crypto;

namespace cryptonote
{
  //---------------------------------------------------------------
  void classify_addresses(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::account_public_address>& change_addr, size_t &num_stdaddresses, size_t &num_subaddresses, account_public_address &single_dest_subaddress)
  {
    num_stdaddresses = 0;
    num_subaddresses = 0;
    std::unordered_set<cryptonote::account_public_address> unique_dst_addresses;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      if (change_addr && dst_entr.addr == change_addr)
        continue;
      if (unique_dst_addresses.count(dst_entr.addr) == 0)
      {
        unique_dst_addresses.insert(dst_entr.addr);
        if (dst_entr.is_subaddress)
        {
          ++num_subaddresses;
          single_dest_subaddress = dst_entr.addr;
        }
        else
        {
          ++num_stdaddresses;
        }
      }
    }
    LOG_PRINT_L2("destinations include " << num_stdaddresses << " standard addresses and " << num_subaddresses << " subaddresses");
  }
  //---------------------------------------------------------------
  bool get_deterministic_output_key(const account_public_address& address, const keypair& tx_key, size_t output_index, crypto::public_key& output_key)
  {
    crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
    return get_deterministic_output_key(address, tx_key, output_index, output_key, derivation);
  }
  //---------------------------------------------------------------
  bool get_deterministic_output_key(const account_public_address& address, const keypair& tx_key, size_t output_index, crypto::public_key& output_key, crypto::key_derivation& derivation)
  {

    // crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
    bool r = crypto::generate_key_derivation(address.m_view_public_key, tx_key.sec, derivation);
    CHECK_AND_ASSERT_MES(r, false, "failed to generate_key_derivation(" << address.m_view_public_key << ", " << tx_key.sec << ")");

    r = crypto::derive_public_key(derivation, output_index, address.m_spend_public_key, output_key);
    CHECK_AND_ASSERT_MES(r, false, "failed to derive_public_key(" << derivation << ", "<< address.m_spend_public_key << ")");

    return true;
  }
  //---------------------------------------------------------------
  keypair get_deterministic_keypair_from_height(uint64_t height)
  {
    keypair k;

    ec_scalar& sec = k.sec;

    for (int i=0; i < 8; i++)
    {
      uint64_t height_byte = height & ((uint64_t)0xFF << (i*8));
      uint8_t byte = height_byte >> i*8;
      sec.data[i] = byte;
    }
    for (int i=8; i < 32; i++)
    {
      sec.data[i] = 0x00;
    }

    generate_keys(k.pub, k.sec, k.sec, true);

    return k;
  }
  //---------------------------------------------------------------
  bool construct_miner_tx(size_t height, size_t median_weight, uint64_t already_generated_coins, size_t current_block_weight, std::map<std::string, uint64_t> fee_map, const account_public_address &miner_address, transaction& tx, const blobdata& extra_nonce, size_t max_outs, uint8_t hard_fork_version, cryptonote::network_type nettype) {
    tx.vin.clear();
    tx.vout.clear();
    tx.extra.clear();

    keypair txkey = keypair::generate(hw::get_device("default"));
    add_tx_pub_key_to_extra(tx, txkey.pub);
    if(!extra_nonce.empty())
      if(!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
        return false;
    if (!sort_tx_extra(tx.extra, tx.extra))
      return false;

    keypair gov_key = get_deterministic_keypair_from_height(height);

    txin_gen in;
    in.height = height;

    uint64_t block_reward;
    if(!get_block_reward(median_weight, current_block_weight, already_generated_coins, block_reward, hard_fork_version))
    {
      LOG_PRINT_L0("Block is too big");
      return false;
    }

#if defined(DEBUG_CREATE_BLOCK_TEMPLATE)
    LOG_PRINT_L1("Creating block template: reward " << block_reward <<
      ", fee " << fee);
#endif

    uint64_t governance_reward = 0;
    uint64_t reserve_reward = 0;
    if (already_generated_coins != 0)
    {
      governance_reward = get_governance_reward(block_reward);
      if (hard_fork_version >= HF_VERSION_DJED) {
        reserve_reward = get_reserve_reward(block_reward);
      }

      block_reward -= reserve_reward;
      block_reward -= governance_reward;

      LOG_PRINT_L1("CREATING BLOCK reserve_reward " << reserve_reward << " height: " << height);
    }
    
    
    block_reward += fee_map["ZEPH"];
    uint64_t summary_amounts = 0;
    CHECK_AND_ASSERT_MES(1 <= max_outs, false, "max_out must be non-zero");

    crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
    crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
    bool r = crypto::generate_key_derivation(miner_address.m_view_public_key, txkey.sec, derivation);
    CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to generate_key_derivation(" << miner_address.m_view_public_key << ", " << txkey.sec << ")");

    r = crypto::derive_public_key(derivation, 0, miner_address.m_spend_public_key, out_eph_public_key);
    CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << 0 << ", "<< miner_address.m_spend_public_key << ")");

    uint64_t amount = block_reward;
    summary_amounts += amount;

    bool use_view_tags = true;
    crypto::view_tag view_tag;
    if (use_view_tags)
      crypto::derive_view_tag(derivation, 0, view_tag);

    tx_out out;
    cryptonote::set_tx_out("ZEPH", amount, out_eph_public_key, use_view_tags, view_tag, out);
    tx.vout.push_back(out);

    cryptonote::address_parse_info governance_wallet_address;
    if (already_generated_coins != 0)
    {
      add_tx_pub_key_to_extra(tx, gov_key.pub);
      cryptonote::get_account_address_from_str(governance_wallet_address, nettype, get_governance_address(nettype));

      crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
      crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
      if (!get_deterministic_output_key(governance_wallet_address.address, gov_key, 1 /* second output in miner tx */, out_eph_public_key, derivation))
      {
        MERROR("Failed to generate deterministic output key for governance wallet output creation");
        return false;
      }

      crypto::view_tag view_tag;
      if (use_view_tags)
        crypto::derive_view_tag(derivation, 1, view_tag);

      tx_out out;
      cryptonote::set_tx_out("ZEPH", governance_reward, out_eph_public_key, use_view_tags, view_tag, out);

      summary_amounts += governance_reward;

      tx.vout.push_back(out);
      CHECK_AND_ASSERT_MES(summary_amounts == (block_reward + governance_reward), false, "Failed to construct miner tx, summary_amounts = " << summary_amounts << " not equal total block_reward = " << (block_reward + governance_reward));
    }

    if (hard_fork_version >= HF_VERSION_DJED) {
      uint64_t idx = 2;
      for (auto &fee_map_entry: fee_map)
      {
        if (fee_map_entry.first == "ZEPH" || fee_map_entry.second == 0)
            continue;
        crypto::key_derivation derivation = AUTO_VAL_INIT(derivation);
        crypto::public_key out_eph_public_key = AUTO_VAL_INIT(out_eph_public_key);
        bool r = crypto::generate_key_derivation(miner_address.m_view_public_key, txkey.sec, derivation);
        CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to generate_key_derivation(" << miner_address.m_view_public_key << ", " << txkey.sec << ")");

        r = crypto::derive_public_key(derivation, idx, miner_address.m_spend_public_key, out_eph_public_key);
        CHECK_AND_ASSERT_MES(r, false, "while creating outs: failed to derive_public_key(" << derivation << ", " << idx << ", "<< miner_address.m_spend_public_key << ")");

        bool use_view_tags = true;
        crypto::view_tag view_tag;
        if (use_view_tags)
          crypto::derive_view_tag(derivation, idx, view_tag);

        tx_out out;
        cryptonote::set_tx_out(fee_map_entry.first, fee_map_entry.second, out_eph_public_key, use_view_tags, view_tag, out);
        tx.vout.push_back(out);
        idx++;
      }
    }

    if (hard_fork_version >= HF_VERSION_DJED) {
      tx.version = 3;
    } else {
      tx.version = 2;
    }

    //lock
    tx.unlock_time = height + CRYPTONOTE_MINED_MONEY_UNLOCK_WINDOW;
    tx.vin.push_back(in);

    tx.invalidate_hashes();

    // LOG_PRINT_L1("miner tx CREATED: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx));
    //LOG_PRINT("MINER_TX generated ok, block_reward=" << print_money(block_reward) << "("  << print_money(block_reward - fee) << "+" << print_money(fee)
    //  << "), current_block_size=" << current_block_size << ", already_generated_coins=" << already_generated_coins << ", tx_id=" << get_transaction_hash(tx), LOG_LEVEL_2);
    return true;
  }
  //---------------------------------------------------------------
  crypto::public_key get_destination_view_key_pub(const std::vector<tx_destination_entry> &destinations, const boost::optional<cryptonote::account_public_address>& change_addr)
  {
    account_public_address addr = {null_pkey, null_pkey};
    size_t count = 0;
    for (const auto &i : destinations)
    {
      if (i.amount == 0)
        continue;
      if (change_addr && i.addr == *change_addr)
        continue;
      if (i.addr == addr)
        continue;
      if (count > 0)
        return null_pkey;
      addr = i.addr;
      ++count;
    }
    if (count == 0 && change_addr)
      return change_addr->m_view_public_key;
    return addr.m_view_public_key;
  }
  //---------------------------------------------------------------
  uint64_t get_governance_reward(uint64_t base_reward)
  {
    return base_reward / 20; // 5% of base reward
  }
  //---------------------------------------------------------------
  uint64_t get_reserve_reward(uint64_t base_reward)
  {
    return base_reward / 5; // 20% of base reward
  }
  //---------------------------------------------------------------
  bool validate_governance_reward_key(uint64_t height, const std::string& governance_wallet_address_str, size_t output_index, const crypto::public_key& output_key, cryptonote::network_type nettype)
  {
    keypair gov_key = get_deterministic_keypair_from_height(height);

    cryptonote::address_parse_info governance_wallet_address;
    cryptonote::get_account_address_from_str(governance_wallet_address, nettype, governance_wallet_address_str);
    crypto::public_key correct_key;

    if (!get_deterministic_output_key(governance_wallet_address.address, gov_key, output_index, correct_key))
    {
      MERROR("Failed to generate deterministic output key for governance wallet output validation");
      return false;
    }

    return correct_key == output_key;
  }
  //---------------------------------------------------------------
  std::string get_governance_address(network_type nettype) {
    if (nettype == TESTNET) {
      return ::config::testnet::GOVERNANCE_WALLET_ADDRESS;
    } else if (nettype == STAGENET) {
      return ::config::stagenet::GOVERNANCE_WALLET_ADDRESS;
    } else {
      return ::config::GOVERNANCE_WALLET_ADDRESS;
    }
  }
  //---------------------------------------------------------------
  bool get_tx_asset_types(const transaction& tx, const crypto::hash &txid, std::string& source, std::string& destination, const bool is_miner_tx) {

    // Clear the source
    std::set<std::string> source_asset_types;
    source = "";
    for (size_t i = 0; i < tx.vin.size(); i++) {
      if (tx.vin[i].type() == typeid(txin_gen)) {
        if (!is_miner_tx) {
          LOG_ERROR("txin_gen detected in non-miner TX. Rejecting..");
          return false;
        }
        source_asset_types.insert("ZEPH");
      } else if (tx.vin[i].type() == typeid(txin_zephyr_key)) {
        source_asset_types.insert(boost::get<txin_zephyr_key>(tx.vin[i]).asset_type);
      } else {
        LOG_ERROR("txin_to_script / txin_to_scripthash detected. Rejecting..");
        return false;
      }
    }

    std::vector<std::string> sat;
    sat.reserve(source_asset_types.size());
    std::copy(source_asset_types.begin(), source_asset_types.end(), std::back_inserter(sat));
    
    // Sanity check that we only have 1 source asset type
    if (sat.size() != 1) {
      LOG_ERROR("Multiple Source Asset types detected. Rejecting..");
      return false;
    }
    source = sat[0];

    // Clear the destination
    std::set<std::string> destination_asset_types;
    destination = "";
    for (const auto &out: tx.vout) {
      std::string output_asset_type;
      bool ok = cryptonote::get_output_asset_type(out, output_asset_type);
      if (!ok) {
        LOG_ERROR("Unexpected output target type found: " << out.target.type().name());
        return false;
      }
      destination_asset_types.insert(output_asset_type);
    }

    std::vector<std::string> dat;
    dat.reserve(destination_asset_types.size());
    std::copy(destination_asset_types.begin(), destination_asset_types.end(), std::back_inserter(dat));
    
    // Check that we have at least 1 destination_asset_type
    if (!dat.size()) {
      LOG_ERROR("No supported destinations asset types detected. Rejecting..");
      return false;
    }
    
    // Handle miner_txs differently - full validation is performed in validate_miner_transaction()
    if (is_miner_tx) {
      destination = "ZEPH";
    } else {
    
      // Sanity check that we only have 1 or 2 destination asset types
      if (dat.size() > 2) {
        LOG_ERROR("Too many (" << dat.size() << ") destination asset types detected in non-miner TX. Rejecting..");
        return false;
      } else if (dat.size() == 1) {
        if (dat[0] != source) {
          LOG_ERROR("Conversion without change detected ([" << source << "] -> [" << dat[0] << "]). Rejecting..");
          return false;
        }
        destination = source;
      } else {
        if (dat[0] == source) {
          destination = dat[1];
        } else if (dat[1] == source) {
          destination = dat[0];
        } else {
          LOG_ERROR("Conversion outputs are incorrect asset types (source asset type not found - [" << source << "] -> [" << dat[0] << "," << dat[1] << "]). Rejecting..");
          return false;
        }
      }
    }
    
    // check both strSource and strDest are supported.
    if (std::find(oracle::ASSET_TYPES.begin(), oracle::ASSET_TYPES.end(), source) == oracle::ASSET_TYPES.end()) {
      LOG_ERROR("Source Asset type " << source << " is not supported! Rejecting..");
      return false;
    }
    if (std::find(oracle::ASSET_TYPES.begin(), oracle::ASSET_TYPES.end(), destination) == oracle::ASSET_TYPES.end()) {
      LOG_ERROR("Destination Asset type " << destination << " is not supported! Rejecting..");
      return false;
    }

    return true;
  }
  //---------------------------------------------------------------
  bool get_tx_type(const std::string& source, const std::string& destination, transaction_type& type) {

    // check both source and destination are supported.
    if (std::find(oracle::ASSET_TYPES.begin(), oracle::ASSET_TYPES.end(), source) == oracle::ASSET_TYPES.end()) {
      LOG_ERROR("Source Asset type " << source << " is not supported! Rejecting..");
      return false;
    }
    if (std::find(oracle::ASSET_TYPES.begin(), oracle::ASSET_TYPES.end(), destination) == oracle::ASSET_TYPES.end()) {
      LOG_ERROR("Destination Asset type " << destination << " is not supported! Rejecting..");
      return false;
    }

    // Find the tx type
    if (source == destination) {
      if (source == "ZEPH") {
        type = transaction_type::TRANSFER;
      } else if (source == "ZEPHUSD") {
        type = transaction_type::STABLE_TRANSFER;
      } else if (source == "ZEPHRSV") {
        type = transaction_type::RESERVE_TRANSFER;
      } else {
        LOG_ERROR("Invalid conversion from " << source << "to" << destination << ". Rejecting..");
        return false;
      }
    } else {
      if (source == "ZEPH" && destination == "ZEPHUSD") {
        type = transaction_type::MINT_STABLE;
      } else if (source == "ZEPHUSD" && destination == "ZEPH") {
        type = transaction_type::REDEEM_STABLE;
      } else if (source == "ZEPH" && destination == "ZEPHRSV") {
        type = transaction_type::MINT_RESERVE;
      } else if (source == "ZEPHRSV" && destination == "ZEPH") {
        type = transaction_type::REDEEM_RESERVE;
      } else {
        LOG_ERROR("Invalid conversion from " << source << "to" << destination << ". Rejecting..");
        return false;
      }
    }

    // Return success to caller
    return true;
  }
  //---------------------------------------------------------------
  void get_circulating_asset_amounts(const std::vector<std::pair<std::string, std::string>>& circ_amounts, multiprecision::uint128_t& zeph_reserve, multiprecision::uint128_t& num_stables, multiprecision::uint128_t& num_reserves)
  {
    zeph_reserve = 0;
    for (auto circ_amount : circ_amounts) {
      if (circ_amount.first == "ZEPH") {
        zeph_reserve = multiprecision::uint128_t(circ_amount.second);
        break;
      }
    }

    num_stables = 0;
    for (auto circ_amount : circ_amounts) {
      if (circ_amount.first == "ZEPHUSD") {
        num_stables = multiprecision::uint128_t(circ_amount.second);
        break;
      }
    }
    num_reserves = 0;
    for (auto circ_amount : circ_amounts) {
      if (circ_amount.first == "ZEPHRSV") {
        num_reserves = multiprecision::uint128_t(circ_amount.second);
        break;
      }
    }
  }
  //---------------------------------------------------------------
  void get_reserve_info(
    const std::vector<std::pair<std::string, std::string>>& circ_amounts,
    const oracle::pricing_record& pr,
    const std::vector<oracle::pricing_record>& pricing_record_history,
    const uint8_t hf_version,
    multiprecision::uint128_t& zeph_reserve,
    multiprecision::uint128_t& num_stables,
    multiprecision::uint128_t& num_reserves,
    multiprecision::uint128_t& assets,
    multiprecision::uint128_t& assets_ma,
    multiprecision::uint128_t& liabilities,
    multiprecision::uint128_t& equity,
    multiprecision::uint128_t& equity_ma,
    double& reserve_ratio,
    double& reserve_ratio_ma
  ){
    get_circulating_asset_amounts(circ_amounts, zeph_reserve, num_stables, num_reserves);

    if (hf_version <= HF_VERSION_PR_UPDATE) {
      multiprecision::cpp_bin_float_quad assets_float = zeph_reserve.convert_to<multiprecision::cpp_bin_float_quad>() * pr.spot;
      multiprecision::cpp_bin_float_quad assets_ma_float = zeph_reserve.convert_to<multiprecision::cpp_bin_float_quad>() * pr.moving_average;
      multiprecision::cpp_bin_float_quad liabilities_float = num_stables.convert_to<multiprecision::cpp_bin_float_quad>();

      if ((assets_float == 0 || assets_ma_float == 0) && liabilities_float == 0) {
        assets = 0;
        assets_ma = 0;
        liabilities = 0;
        equity = 0;
        equity_ma = 0;
        reserve_ratio = 0;
        reserve_ratio_ma = 0;
        return;
      }

      multiprecision::cpp_bin_float_quad reserve_ratio_128 = assets_float / liabilities_float;
      multiprecision::cpp_bin_float_quad reserve_ratio_ma_128 = assets_ma_float / liabilities_float;

      assets_float /= COIN;
      assets_ma_float /= COIN;
      assets = assets_float.convert_to<multiprecision::uint128_t>();
      assets_ma = assets_ma_float.convert_to<multiprecision::uint128_t>();
      liabilities = liabilities_float.convert_to<multiprecision::uint128_t>();
      equity = assets > liabilities ? assets - liabilities : 0;
      equity_ma = assets_ma > liabilities ? assets_ma - liabilities : 0;

      reserve_ratio_128 /= COIN;
      reserve_ratio_ma_128 /= COIN;
      reserve_ratio = reserve_ratio_128.convert_to<double>();
      reserve_ratio_ma = reserve_ratio_ma_128.convert_to<double>();
    } else {
      assets = zeph_reserve * pr.spot;
      liabilities = num_stables;
      if (num_stables == 0) {
        assets /= COIN;
        equity = assets;
        equity_ma = 0;
        reserve_ratio = 0;
        reserve_ratio_ma = 0;
        return;
      }

      multiprecision::uint128_t reserve_ratio_int = assets / num_stables;
      multiprecision::uint128_t reserve_ratio_ma_int = get_moving_average_reserve_ratio(pricing_record_history, (uint64_t)reserve_ratio_int);

      reserve_ratio = reserve_ratio_int.convert_to<double>();
      reserve_ratio /= COIN;
      reserve_ratio_ma = reserve_ratio_ma_int.convert_to<double>();
      reserve_ratio_ma /= COIN;
      assets /= COIN;
      equity = assets > liabilities ? assets - liabilities : 0;
      assets_ma = 0;
      equity_ma = 0;
    }
  }
  double get_spot_reserve_ratio(const std::vector<std::pair<std::string, std::string>>& circ_amounts, const oracle::pricing_record& pr)
  {
    return get_reserve_ratio(circ_amounts, pr.spot);
  }
  double get_ma_reserve_ratio(const std::vector<std::pair<std::string, std::string>>& circ_amounts, const oracle::pricing_record& pr)
  {
    return get_reserve_ratio(circ_amounts, pr.moving_average);
  }
  double get_reserve_ratio(const std::vector<std::pair<std::string, std::string>>& circ_amounts, const uint64_t oracle_price)
  {
    multiprecision::uint128_t zeph_reserve, num_stables, num_reserves;
    get_circulating_asset_amounts(circ_amounts, zeph_reserve, num_stables, num_reserves);

    multiprecision::cpp_bin_float_quad assets = zeph_reserve.convert_to<multiprecision::cpp_bin_float_quad>() * oracle_price;
    multiprecision::cpp_bin_float_quad liabilities = num_stables.convert_to<multiprecision::cpp_bin_float_quad>();
    if (assets == 0 && liabilities == 0) return 0;

    multiprecision::cpp_bin_float_quad reserve_ratio = assets / liabilities;
    reserve_ratio /= COIN;
    if (reserve_ratio > std::numeric_limits<double>::max()) return std::numeric_limits<double>::infinity();
    return (double)reserve_ratio;
  }
  uint64_t get_pr_reserve_ratio(const std::vector<std::pair<std::string, std::string>>& circ_amounts, const uint64_t oracle_price)
  {
    multiprecision::uint128_t zeph_reserve, num_stables, num_reserves;
    get_circulating_asset_amounts(circ_amounts, zeph_reserve, num_stables, num_reserves);

    multiprecision::uint128_t assets = zeph_reserve * oracle_price;
    if (num_stables == 0) return 0;

    multiprecision::uint128_t reserve_ratio = assets / num_stables;
    reserve_ratio -= (reserve_ratio % 10000);
    if (reserve_ratio > std::numeric_limits<uint64_t>::max()) return 0;
    return (uint64_t)reserve_ratio;
  }
  //---------------------------------------------------------------
  bool reserve_ratio_satisfied(const std::vector<std::pair<std::string, std::string>>& circ_amounts, const std::vector<oracle::pricing_record>& pricing_record_history, const oracle::pricing_record& pr, const transaction_type& tx_type, multiprecision::int128_t tally_zeph, multiprecision::int128_t tally_stables, multiprecision::int128_t tally_reserves, const uint8_t hf_version)
  {
    std::string error_reason;
    return reserve_ratio_satisfied(circ_amounts, pricing_record_history, pr, tx_type, tally_zeph, tally_stables, tally_reserves, error_reason, hf_version);
  }
  //---------------------------------------------------------------
  bool reserve_ratio_satisfied(const std::vector<std::pair<std::string, std::string>>& circ_amounts, const std::vector<oracle::pricing_record>& pricing_record_history, const oracle::pricing_record& pr, const transaction_type& tx_type, multiprecision::int128_t tally_zeph, multiprecision::int128_t tally_stables, multiprecision::int128_t tally_reserves, std::string& error_reason, const uint8_t hf_version)
  {
    if (pr.has_missing_rates(hf_version)) {
      error_reason = "Reserve ratio cannot be calculated. Pricing record is missing rates.";
      LOG_ERROR(error_reason);
      return false;
    }

    multiprecision::uint128_t zeph_reserve, num_stables, num_reserves;
    get_circulating_asset_amounts(circ_amounts, zeph_reserve, num_stables, num_reserves);

    // Early exit if no ZEPH in the reserve
    if (zeph_reserve == 0) {
      if (tx_type == transaction_type::MINT_RESERVE) {
        return true;
      }
      error_reason = "Reserve ratio not satisfied. No ZEPH in the reserve.";
      LOG_ERROR(error_reason);
      return false;
    }

    if (hf_version >= HF_VERSION_V5) {
      multiprecision::int128_t assets = zeph_reserve.convert_to<multiprecision::int128_t>() + tally_zeph;
      if (assets < 0) {
        error_reason = "Reserve ratio not satisfied. Zeph reserve would be negative.";
        LOG_ERROR(error_reason);
        return false;
      }

      multiprecision::int128_t liabilities = num_stables.convert_to<multiprecision::int128_t>() + tally_stables;
      if (liabilities < 0) {
        error_reason = "Reserve ratio not satisfied. Liabilities would be negative.";
        LOG_ERROR(error_reason);
        return false;
      }

      multiprecision::int128_t total_reserve_coins = num_reserves.convert_to<multiprecision::int128_t>() + tally_reserves;
      if (total_reserve_coins < 0) {
        error_reason = "Reserve ratio not satisfied. Total reserve coins would be negative.";
        LOG_ERROR(error_reason);
        return false;
      }

      if (assets == 0 && liabilities == 0) {
        error_reason = "Reserve ratio not satisfied. Assets and liabilities are both zero.";
        LOG_ERROR(error_reason);
        return false;
      }

      multiprecision::int128_t assets_spot = assets * pr.spot;
      if (assets != 0 && assets_spot == 0) {
        error_reason = "Reserve ratio not satisfied. Error calculating assets.";
        LOG_ERROR(error_reason);
        return false;
      }

      multiprecision::int128_t reserve_ratio_spot;
      multiprecision::int128_t reserve_ratio_MA;
      if (liabilities == 0) {
        reserve_ratio_spot = std::numeric_limits<multiprecision::int128_t>::infinity();
        reserve_ratio_MA = std::numeric_limits<multiprecision::int128_t>::infinity();
      } else {
        reserve_ratio_spot = assets_spot / liabilities;
        reserve_ratio_MA = get_moving_average_reserve_ratio(pricing_record_history, (uint64_t)reserve_ratio_spot);
      }

      if (reserve_ratio_spot < 0 || reserve_ratio_MA < 0) {
        error_reason = "Reserve ratio not satisfied. Reserve ratio would be negative.";
        LOG_ERROR(error_reason);
        return false;
      }

      const uint64_t RESERVE_RATIO_MIN = 4 * COIN;
      const uint64_t RESERVE_RATIO_MAX = 8 * COIN;

      if (tx_type == transaction_type::MINT_STABLE) {
        // Make sure the reserve ratio is at least 4.0
        if (reserve_ratio_spot < RESERVE_RATIO_MIN) {
          error_reason = "Spot reserve ratio not satisfied. New reserve ratio would be " + print_money((uint64_t)reserve_ratio_spot) + " which is less than minimum 4.0";
          LOG_ERROR(error_reason);
          return false;
        }
        if (reserve_ratio_MA < RESERVE_RATIO_MIN) {
          error_reason = "MA reserve ratio not satisfied. New reserve ratio would be " + print_money((uint64_t)reserve_ratio_MA) + " which is less than minimum 4.0";
          LOG_ERROR(error_reason);
          return false;
        }
        return true;
      }

      if (tx_type == transaction_type::REDEEM_STABLE) {
        if (assets == 0) {
          error_reason = "Reserve ratio not satisfied. Assets are zero.";
          LOG_ERROR(error_reason);
          return false;
        }
        return true;
      }

      if (tx_type == transaction_type::MINT_RESERVE) {
        // If there is less than 100 circulating stablecoins, then reserve coin minting is allowed
        const uint64_t threshold_num_stables = 100 * COIN;
        if (liabilities < threshold_num_stables) {
          return true;
        }
        // Make sure the reserve ratio has not exceeded max of 8.0
        if (reserve_ratio_spot >= RESERVE_RATIO_MAX) {
          error_reason = "Spot reserve ratio not satisfied. New reserve ratio would be " + print_money((uint64_t)reserve_ratio_spot) + " which is above the maximum 8.0";
          LOG_ERROR(error_reason);
          return false;
        }
        if (reserve_ratio_MA >= RESERVE_RATIO_MAX) {
          error_reason = "MA reserve ratio not satisfied. New reserve ratio would be " + print_money((uint64_t)reserve_ratio_MA) + " which is above the maximum 8.0";
          LOG_ERROR(error_reason);
          return false;
        }
        return true;
      }

      if (tx_type == transaction_type::REDEEM_RESERVE) {
        // Make sure the reserve ratio is at least 4.0
        if (reserve_ratio_spot < RESERVE_RATIO_MIN) {
          error_reason = "Reserve ratio not satisfied. New reserve ratio would be " + print_money((uint64_t)reserve_ratio_spot) + " which is less than the minimum 4.0";
          LOG_ERROR(error_reason);
          return false;
        }
        if (reserve_ratio_MA < RESERVE_RATIO_MIN) {
          error_reason = "Reserve ratio not satisfied. New reserve ratio would be " + print_money((uint64_t)reserve_ratio_MA) + " which is less than the minimum 4.0";
          LOG_ERROR(error_reason);
          return false;
        }
        return true;
      }

      error_reason = "Reserve ratios not satisfied. Spot: " + print_money((uint64_t)reserve_ratio_spot) + " | MA: " + print_money((uint64_t)reserve_ratio_MA);
      LOG_ERROR(error_reason);
      return false;
    } else {
      multiprecision::cpp_bin_float_quad assets = zeph_reserve.convert_to<multiprecision::cpp_bin_float_quad>() + tally_zeph.convert_to<multiprecision::cpp_bin_float_quad>();
      if (assets < 0) {
        error_reason = "Reserve ratio not satisfied. Zeph reserve would be negative.";
        LOG_ERROR(error_reason);
        return false;
      }

      multiprecision::cpp_bin_float_quad liabilities = num_stables.convert_to<multiprecision::cpp_bin_float_quad>() + tally_stables.convert_to<multiprecision::cpp_bin_float_quad>();
      if (liabilities < 0) {
        error_reason = "Reserve ratio not satisfied. Liabilities would be negative.";
        LOG_ERROR(error_reason);
        return false;
      }

      multiprecision::cpp_bin_float_quad total_reserve_coins = num_reserves.convert_to<multiprecision::cpp_bin_float_quad>() + tally_reserves.convert_to<multiprecision::cpp_bin_float_quad>();
      if (total_reserve_coins < 0) {
        error_reason = "Reserve ratio not satisfied. Total reserve coins would be negative.";
        LOG_ERROR(error_reason);
        return false;
      }

      if (assets == 0 && liabilities == 0) {
        error_reason = "Reserve ratio not satisfied. Assets and liabilities are both zero.";
        LOG_ERROR(error_reason);
        return false;
      }

      multiprecision::cpp_bin_float_quad assets_spot = assets * pr.spot;
      multiprecision::cpp_bin_float_quad assets_MA = assets * pr.moving_average;
      if (assets != 0 && (assets_spot == 0 || assets_MA == 0)) {
        error_reason = "Reserve ratio not satisfied. Error calculating assets.";
        LOG_ERROR(error_reason);
        return false;
      }

      multiprecision::cpp_bin_float_quad reserve_ratio_spot = assets_spot / liabilities;
      multiprecision::cpp_bin_float_quad reserve_ratio_MA = assets_MA / liabilities;

      reserve_ratio_spot /= COIN;
      reserve_ratio_MA /= COIN;

      if (boost::math::isnan(reserve_ratio_spot) || boost::math::isnan(reserve_ratio_MA)) {
        error_reason = "Reserve ratio not satisfied. Error calculating reserve ratio.";
        LOG_ERROR(error_reason);
        return false;
      }
      if (reserve_ratio_spot < 0 || reserve_ratio_MA < 0) {
        error_reason = "Reserve ratio not satisfied. Reserve ratio would be negative.";
        LOG_ERROR(error_reason);
        return false;
      }

      if (tx_type == transaction_type::MINT_STABLE) {
        // Make sure the reserve ratio is at least 4.0
        if (reserve_ratio_spot < 4.0) {
          error_reason = "Spot reserve ratio not satisfied. New reserve ratio would be " + std::to_string((double)reserve_ratio_spot) + " which is less than minimum 4.0";
          LOG_ERROR(error_reason);
          return false;
        }
        if (reserve_ratio_MA < 4.0) {
          error_reason = "MA reserve ratio not satisfied. New reserve ratio would be " + std::to_string((double)reserve_ratio_MA) + " which is less than minimum 4.0";
          LOG_ERROR(error_reason);
          return false;
        }
        return true;
      }

      if (tx_type == transaction_type::REDEEM_STABLE) {
        if (assets == 0) {
          error_reason = "Reserve ratio not satisfied. Assets are zero.";
          LOG_ERROR(error_reason);
          return false;
        }
        return true;
      }

      if (tx_type == transaction_type::MINT_RESERVE) {
        // If there is less than 100 circulating stablecoins, then reserve coin minting is allowed
        const uint64_t threshold_num_stables = 100 * COIN;
        if (liabilities < threshold_num_stables) {
          return true;
        }
        // Make sure the reserve ratio has not exceeded max of 8.0
        if (reserve_ratio_spot >= 8.0) {
          error_reason = "Spot reserve ratio not satisfied. New reserve ratio would be " + std::to_string((double)reserve_ratio_spot) + " which is above the maximum 8.0";
          LOG_ERROR(error_reason);
          return false;
        }
        if (reserve_ratio_MA >= 8.0) {
          error_reason = "MA reserve ratio not satisfied. New reserve ratio would be " + std::to_string((double)reserve_ratio_MA) + " which is above the maximum 8.0";
          LOG_ERROR(error_reason);
          return false;
        }
        return true;
      }

      if (tx_type == transaction_type::REDEEM_RESERVE) {
        // Make sure the reserve ratio is at least 4.0
        if (reserve_ratio_spot < 4.0) {
          error_reason = "Reserve ratio not satisfied. New reserve ratio would be " + std::to_string((double)reserve_ratio_spot) + " which is less than the minimum 4.0";
          LOG_ERROR(error_reason);
          return false;
        }
        if (reserve_ratio_MA < 4.0) {
          error_reason = "Reserve ratio not satisfied. New reserve ratio would be " + std::to_string((double)reserve_ratio_MA) + " which is less than the minimum 4.0";
          LOG_ERROR(error_reason);
          return false;
        }
        return true;
      }

      error_reason = "Reserve ratios not satisfied. Spot: " + std::to_string((double)reserve_ratio_spot) + " | MA: " + std::to_string((double)reserve_ratio_MA);
      LOG_ERROR(error_reason);
      return false;
    }
  }
   //---------------------------------------------------------------
  uint64_t get_stable_coin_price(const std::vector<std::pair<std::string, std::string>>& circ_amounts, uint64_t oracle_price)
  {
    if (oracle_price <= 0) return 0;

    multiprecision::uint128_t rate_128 = COIN;
    rate_128 *= COIN;
    rate_128 /= oracle_price;
    rate_128 -= (rate_128 % 10000);

    if (rate_128 > std::numeric_limits<uint64_t>::max()) {
      MWARNING("overflow detected in stable coin price calculation.");
      rate_128 = 0;
    }
    uint64_t rate = rate_128.convert_to<uint64_t>();

    multiprecision::uint128_t zeph_reserve, num_stables, num_reserves;
    get_circulating_asset_amounts(circ_amounts, zeph_reserve, num_stables, num_reserves);

    if (num_stables == 0) {
      return rate;
    }

    // Calculate the reserve ratio
    multiprecision::cpp_bin_float_quad assets = zeph_reserve.convert_to<multiprecision::cpp_bin_float_quad>();
    assets *= oracle_price;
    multiprecision::cpp_bin_float_quad reserve_ratio_atomized = assets / num_stables.convert_to<multiprecision::cpp_bin_float_quad>();
    multiprecision::cpp_bin_float_quad reserve_ratio = reserve_ratio_atomized / COIN;

    if (reserve_ratio < 1.0) {
      zeph_reserve *= COIN;
      multiprecision::uint128_t worst_case_stable_rate = zeph_reserve / num_stables;
      worst_case_stable_rate -= (worst_case_stable_rate % 10000);
      if (worst_case_stable_rate > std::numeric_limits<uint64_t>::max()) {
        MWARNING("overflow detected in stablecoin price calculation.");
        worst_case_stable_rate = 0;
      }
      return worst_case_stable_rate.convert_to<uint64_t>();
    }

    return rate;
  }
  //---------------------------------------------------------------
  uint64_t get_reserve_coin_price(const std::vector<std::pair<std::string, std::string>>& circ_amounts, uint64_t exchange_rate)
  {
    if (exchange_rate <= 0) return 0;

    multiprecision::uint128_t zeph_reserve, num_stables, num_reserves;
    get_circulating_asset_amounts(circ_amounts, zeph_reserve, num_stables, num_reserves);
    
    uint64_t price_r_min = 500000000000;
    if (num_reserves == 0) {
      MDEBUG("No reserve amount detected. Using price_r_min..");
      return price_r_min;
    }

    multiprecision::cpp_bin_float_quad assets = zeph_reserve.convert_to<multiprecision::cpp_bin_float_quad>();
    multiprecision::cpp_bin_float_quad liabilities = num_stables.convert_to<multiprecision::cpp_bin_float_quad>();

    liabilities *= COIN;
    liabilities /= exchange_rate;

    multiprecision::cpp_bin_float_quad equity = assets - liabilities;

    if (equity < 0) {
      return price_r_min;
    }

    equity *= COIN;
    multiprecision::cpp_bin_float_quad reserve_coin_price_float = equity / num_reserves.convert_to<multiprecision::cpp_bin_float_quad>();

    if (reserve_coin_price_float > std::numeric_limits<uint64_t>::max()) {
      MWARNING("overflow detected in reserve coin price calculation.");
      return 0;
    }

    uint64_t reserve_coin_price = reserve_coin_price_float.convert_to<uint64_t>();
    reserve_coin_price -= (reserve_coin_price % 10000);
    
    return std::max(reserve_coin_price, price_r_min);
  }
  //---------------------------------------------------------------
  uint64_t get_moving_average_price(const std::vector<oracle::pricing_record>& pricing_record_history, uint64_t spot_price)
  {
    if (pricing_record_history.size() < 719) {
      return 0;
    }

    uint64_t sum = 0;
    int first_pr_index = pricing_record_history.size() - 719;
    for (int i = pricing_record_history.size() - 1; i >= first_pr_index; i--) {
      sum += pricing_record_history[i].spot;
    }

    uint64_t moving_average = (sum + spot_price) / 720;
    moving_average -= (moving_average % 10000);
    return moving_average;
  }
  //---------------------------------------------------------------
  uint64_t get_moving_average_stable_coin_price(const std::vector<oracle::pricing_record>& pricing_record_history, uint64_t stable_price)
  {
    if (pricing_record_history.size() < 719) {
      return 0;
    }

    uint64_t sum = 0;
    int first_pr_index = pricing_record_history.size() - 719;
    for (int i = pricing_record_history.size() - 1; i >= first_pr_index; i--) {
      sum += pricing_record_history[i].stable;
    }

    uint64_t moving_average = (sum + stable_price) / 720;
    moving_average -= (moving_average % 10000);
    return moving_average;
  }
  //---------------------------------------------------------------
  uint64_t get_moving_average_reserve_coin_price(const std::vector<oracle::pricing_record>& pricing_record_history, uint64_t reserve_price)
  {
    if (pricing_record_history.size() < 719) {
      return 0;
    }

    uint64_t sum = 0;
    int first_pr_index = pricing_record_history.size() - 719;
    for (int i = pricing_record_history.size() - 1; i >= first_pr_index; i--) {
      sum += pricing_record_history[i].reserve;
    }

    uint64_t moving_average = (sum + reserve_price) / 720;
    moving_average -= (moving_average % 10000);
    return moving_average;
  }
  //---------------------------------------------------------------
  uint64_t get_moving_average_reserve_ratio(const std::vector<oracle::pricing_record>& pricing_record_history, uint64_t reserve_ratio)
  {
    if (pricing_record_history.size() < 719) {
      return 0;
    }

    uint64_t sum = 0;
    int first_pr_index = pricing_record_history.size() - 719;
    for (int i = pricing_record_history.size() - 1; i >= first_pr_index; i--) {
      sum += pricing_record_history[i].reserve_ratio;
    }

    uint64_t moving_average = (sum + reserve_ratio) / 720;
    moving_average -= (moving_average % 10000);
    return moving_average;
  }
  //---------------------------------------------------------------
  // ZEPH -> ZEPHRSV
  uint64_t zeph_to_zephrsv(const uint64_t amount, const oracle::pricing_record& pr, const uint8_t hf_version)
  {
    multiprecision::uint128_t amount_128 = amount;
    multiprecision::uint128_t reserve_coin_price = std::max(pr.reserve, pr.reserve_ma);

    // for rct precision
    multiprecision::uint128_t rate_128 = COIN;
    rate_128 *= COIN;
    rate_128 /= reserve_coin_price;
    multiprecision::uint128_t conversion_fee;
    if (hf_version >= HF_VERSION_V5) {
      conversion_fee = rate_128 / 100;       // 1% fee
    } else {
      conversion_fee = 0;                    // no fee
    }
    rate_128 -= conversion_fee;
    rate_128 -= (rate_128 % 10000);

    multiprecision::uint128_t reserve_amount_128 = amount_128 * rate_128;
    reserve_amount_128 /= COIN;

    if (reserve_amount_128 > std::numeric_limits<uint64_t>::max()) {
      MWARNING("overflow detected in reserve amount calculation.");
      reserve_amount_128 = 0;
    }

    return reserve_amount_128.convert_to<uint64_t>();
  }
  //---------------------------------------------------------------
  // ZEPHRSV -> ZEPH
  uint64_t zephrsv_to_zeph(const uint64_t amount, const oracle::pricing_record& pr, const uint8_t hf_version)
  {
    multiprecision::uint128_t amount_128 = amount;
    multiprecision::uint128_t reserve_coin_price = std::min(pr.reserve, pr.reserve_ma);
    multiprecision::uint128_t conversion_fee;
    if (hf_version >= HF_VERSION_V5) {
      conversion_fee = reserve_coin_price / 100;       // 1% fee
    } else {
      conversion_fee = (reserve_coin_price * 2) / 100; // 2% fee
    }
    reserve_coin_price -= conversion_fee;
    reserve_coin_price -= (reserve_coin_price % 10000);

    multiprecision::uint128_t reserve_amount_128 = amount_128 * reserve_coin_price;
    reserve_amount_128 /= COIN;

    if (reserve_amount_128 > std::numeric_limits<uint64_t>::max()) {
      MWARNING("overflow detected in zeph amount from reserve amount calculation.");
      reserve_amount_128 = 0;
    }

    return reserve_amount_128.convert_to<uint64_t>();
  }
  //---------------------------------------------------------------
  // ZEPH -> ZEPHUSD
  uint64_t zeph_to_zephusd(const uint64_t amount, const oracle::pricing_record& pr, const uint8_t hf_version)
  {
    multiprecision::uint128_t amount_128 = amount;
    multiprecision::uint128_t exchange_128 = std::max(pr.stable, pr.stable_ma);

    multiprecision::uint128_t rate_128 = COIN;
    rate_128 *= COIN;
    rate_128 /= exchange_128;

    multiprecision::uint128_t conversion_fee;
    if (hf_version >= HF_VERSION_V5) {
      conversion_fee = rate_128 / 1000;      // 0.1% fee
    } else {
      conversion_fee = (rate_128 * 2) / 100; // 2% fee
    }
    rate_128 -= conversion_fee;
    rate_128 -= (rate_128 % 10000);

    multiprecision::uint128_t stable_128 = amount_128 * rate_128;
    stable_128 /= COIN;

    if (stable_128 > std::numeric_limits<uint64_t>::max()) {
      MWARNING("overflow detected in stable amount calculation.");
      stable_128 = 0;
    }

    return stable_128.convert_to<uint64_t>();
  }
  //---------------------------------------------------------------
  // ZEPHUSD -> ZEPH
  uint64_t zephusd_to_zeph(const uint64_t amount, const oracle::pricing_record& pr, const uint8_t hf_version)
  {
    multiprecision::uint128_t stable_128 = amount;
    multiprecision::uint128_t exchange_128 = std::min(pr.stable, pr.stable_ma);
    multiprecision::uint128_t conversion_fee;
    if (hf_version >= HF_VERSION_V5) {
      conversion_fee = exchange_128 / 1000;      // 0.1% fee
    } else {
      conversion_fee = (exchange_128 * 2) / 100; // 2% fee
    }
    exchange_128 -= conversion_fee;
    exchange_128 -= (exchange_128 % 10000);
    
    multiprecision::uint128_t zeph_128 = stable_128 * exchange_128;
    zeph_128 /= COIN;

    if (zeph_128 > std::numeric_limits<uint64_t>::max()) {
      MWARNING("overflow detected in zeph amount calculation.");
      zeph_128 = 0;
    }

    return zeph_128.convert_to<uint64_t>();
  }
  //---------------------------------------------------------------
  uint64_t zeph_to_asset_fee(const uint64_t zeph_fee, const uint64_t exchange_rate)
  {
    multiprecision::uint128_t zeph_fee_128 = zeph_fee;
    multiprecision::uint128_t rate_128 = COIN;
    rate_128 *= COIN;
    rate_128 /= exchange_rate;
    rate_128 -= (rate_128 % 10000);

    multiprecision::uint128_t asset_fee = zeph_fee_128 * rate_128;
    asset_fee /= COIN;
    if (asset_fee > std::numeric_limits<uint64_t>::max()) {
      MWARNING("overflow detected in zeph_to_asset_fee calculation.");
      asset_fee = 0;
    }
    return asset_fee.convert_to<uint64_t>();
  }
  //---------------------------------------------------------------
  uint64_t asset_to_zeph_fee(const uint64_t asset_fee, const uint64_t exchange_rate)
  {
    multiprecision::uint128_t asset_fee_128 = asset_fee;
    multiprecision::uint128_t zeph_fee = asset_fee_128 * exchange_rate;
    zeph_fee /= COIN;
    if (zeph_fee > std::numeric_limits<uint64_t>::max()) {
      MWARNING("overflow detected in asset_to_zeph_fee calculation.");
      zeph_fee = 0;
    }
    return zeph_fee.convert_to<uint64_t>();
  }
  //---------------------------------------------------------------------------------
  uint64_t get_fee_in_zeph_equivalent(const std::string& fee_asset, uint64_t fee_amount, const oracle::pricing_record& pr, const uint8_t hf_version)
  {
    if (fee_asset == "ZEPH" || pr.has_missing_rates(hf_version)) {
      return fee_amount;
    } else if (fee_asset == "ZEPHUSD") {
      return asset_to_zeph_fee(fee_amount, pr.stable_ma);
    } else if (fee_asset == "ZEPHRSV") {
      return asset_to_zeph_fee(fee_amount, pr.reserve_ma);
    }

    return fee_amount;
  }
    //---------------------------------------------------------------------------------
  uint64_t get_fee_in_asset_equivalent(const std::string& to_asset_type, uint64_t fee_amount, const oracle::pricing_record& pr, const uint8_t hf_version)
  {
    if (to_asset_type == "ZEPH" || pr.has_missing_rates(hf_version)) {
      return fee_amount;
    } else if (to_asset_type == "ZEPHUSD") {
      return zeph_to_asset_fee(fee_amount, pr.stable_ma);
    } else if (to_asset_type == "ZEPHRSV") {
      return zeph_to_asset_fee(fee_amount, pr.reserve_ma);
    }

    return fee_amount;
  }
  //----------------------------------------------------------------------------------------------------
  bool tx_pr_height_valid(const uint64_t current_height, const uint64_t pr_height, const crypto::hash& tx_hash) {
    if (pr_height >= current_height) {
      return false;
    }
    if ((current_height - PRICING_RECORD_VALID_BLOCKS) > pr_height) {
       return false;
    }
    return true;
  }
  //---------------------------------------------------------------
  bool construct_tx_with_tx_key(
    const account_keys& sender_account_keys,
    const std::unordered_map<crypto::public_key, subaddress_index>& subaddresses,
    std::vector<tx_source_entry>& sources,
    std::vector<tx_destination_entry>& destinations,
    const boost::optional<cryptonote::account_public_address>& change_addr,
    const std::vector<uint8_t> &extra,
    transaction& tx,
    const std::string& source_asset,
    const std::string& dest_asset,
    const uint64_t current_height, 
    const uint8_t hf_version,
    const oracle::pricing_record& pr,
    const std::vector<std::pair<std::string, std::string>> circ_amounts,
    const std::vector<oracle::pricing_record>& pricing_record_history,
    uint64_t unlock_time,
    const crypto::secret_key &tx_key,
    const std::vector<crypto::secret_key> &additional_tx_keys,
    bool rct,
    const rct::RCTConfig &rct_config,
    bool shuffle_outs,
    bool use_view_tags
  )
  {
    hw::device &hwdev = sender_account_keys.get_device();

    if (sources.empty())
    {
      LOG_ERROR("Empty sources");
      return false;
    }

    std::vector<rct::key> amount_keys;
    tx.set_null();
    amount_keys.clear();

    if (hf_version >= HF_VERSION_DJED) {
      tx.version = 3;
    } else {
      tx.version = 2;
    }
    tx.unlock_time = unlock_time;

    tx.extra = extra;
    crypto::public_key txkey_pub;

    // check both strSource and strDest are supported.
    if (std::find(oracle::ASSET_TYPES.begin(), oracle::ASSET_TYPES.end(), source_asset) == oracle::ASSET_TYPES.end()) {
      LOG_ERROR("Unsupported source asset type " << source_asset);
      return false;
    }
    if (std::find(oracle::ASSET_TYPES.begin(), oracle::ASSET_TYPES.end(), dest_asset) == oracle::ASSET_TYPES.end()) {
      LOG_ERROR("Unsupported destination asset type " << dest_asset);
      return false;
    }


    if (source_asset != dest_asset) {
      tx.pricing_record_height = current_height;
    } else {
      tx.pricing_record_height = 0;
    }

    // if we have a stealth payment id, find it and encrypt it with the tx key now
    std::vector<tx_extra_field> tx_extra_fields;
    if (parse_tx_extra(tx.extra, tx_extra_fields))
    {
      bool add_dummy_payment_id = true;
      tx_extra_nonce extra_nonce;
      if (find_tx_extra_field_by_type(tx_extra_fields, extra_nonce))
      {
        crypto::hash payment_id = null_hash;
        crypto::hash8 payment_id8 = null_hash8;
        if (get_encrypted_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id8))
        {
          LOG_PRINT_L2("Encrypting payment id " << payment_id8);
          crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
          if (view_key_pub == null_pkey)
          {
            LOG_ERROR("Destinations have to have exactly one output to support encrypted payment ids");
            return false;
          }

          if (!hwdev.encrypt_payment_id(payment_id8, view_key_pub, tx_key))
          {
            LOG_ERROR("Failed to encrypt payment id");
            return false;
          }

          std::string extra_nonce;
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id8);
          remove_field_from_tx_extra(tx.extra, typeid(tx_extra_nonce));
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add encrypted payment id to tx extra");
            return false;
          }
          LOG_PRINT_L1("Encrypted payment ID: " << payment_id8);
          add_dummy_payment_id = false;
        }
        else if (get_payment_id_from_tx_extra_nonce(extra_nonce.nonce, payment_id))
        {
          add_dummy_payment_id = false;
        }
      }

      // we don't add one if we've got more than the usual 1 destination plus change
      if (destinations.size() > 2)
        add_dummy_payment_id = false;

      if (add_dummy_payment_id)
      {
        // if we have neither long nor short payment id, add a dummy short one,
        // this should end up being the vast majority of txes as time goes on
        std::string extra_nonce;
        crypto::hash8 payment_id8 = null_hash8;
        crypto::public_key view_key_pub = get_destination_view_key_pub(destinations, change_addr);
        if (view_key_pub == null_pkey)
        {
          LOG_ERROR("Failed to get key to encrypt dummy payment id with");
        }
        else
        {
          hwdev.encrypt_payment_id(payment_id8, view_key_pub, tx_key);
          set_encrypted_payment_id_to_tx_extra_nonce(extra_nonce, payment_id8);
          if (!add_extra_nonce_to_tx_extra(tx.extra, extra_nonce))
          {
            LOG_ERROR("Failed to add dummy encrypted payment id to tx extra");
            // continue anyway
          }
        }
      }
    }
    else
    {
      MWARNING("Failed to parse tx extra");
      tx_extra_fields.clear();
    }

    struct input_generation_context_data
    {
      keypair in_ephemeral;
    };
    std::vector<input_generation_context_data> in_contexts;

    uint64_t summary_inputs_money = 0;
    //fill inputs
    int idx = -1;
    for(const tx_source_entry& src_entr:  sources)
    {
      ++idx;
      if(src_entr.real_output >= src_entr.outputs.size())
      {
        LOG_ERROR("real_output index (" << src_entr.real_output << ")bigger than output_keys.size()=" << src_entr.outputs.size());
        return false;
      }
     
      summary_inputs_money += src_entr.amount;    

      //key_derivation recv_derivation;
      in_contexts.push_back(input_generation_context_data());
      keypair& in_ephemeral = in_contexts.back().in_ephemeral;
      crypto::key_image img;
      const auto& out_key = reinterpret_cast<const crypto::public_key&>(src_entr.outputs[src_entr.real_output].second.dest);
      if(!generate_key_image_helper(sender_account_keys, subaddresses, out_key, src_entr.real_out_tx_key, src_entr.real_out_additional_tx_keys, src_entr.real_output_in_tx_index, in_ephemeral,img, hwdev))
      {
        LOG_ERROR("Key image generation failed!");
        return false;
      }

      //check that derivated key is equal with real output key
      if(!(in_ephemeral.pub == src_entr.outputs[src_entr.real_output].second.dest) )
      {
        LOG_ERROR("derived public key mismatch with output public key at index " << idx << ", real out " << src_entr.real_output << "! "<< ENDL << "derived_key:"
          << string_tools::pod_to_hex(in_ephemeral.pub) << ENDL << "real output_public_key:"
          << string_tools::pod_to_hex(src_entr.outputs[src_entr.real_output].second.dest) );
        LOG_ERROR("amount " << src_entr.amount << ", rct " << src_entr.rct);
        LOG_ERROR("tx pubkey " << src_entr.real_out_tx_key << ", real_output_in_tx_index " << src_entr.real_output_in_tx_index);
        return false;
      }

      //put key image into tx input
      txin_zephyr_key input_to_key;
      input_to_key.amount = src_entr.amount;
      input_to_key.k_image = img;
      input_to_key.asset_type = src_entr.asset_type;

      //fill outputs array and use relative offsets
      for(const tx_source_entry::output_entry& out_entry: src_entr.outputs)
        input_to_key.key_offsets.push_back(out_entry.first);

      input_to_key.key_offsets = absolute_output_offsets_to_relative(input_to_key.key_offsets);
      tx.vin.push_back(input_to_key);
    }

    transaction_type tx_type;
    if (!get_tx_type(source_asset, dest_asset, tx_type)) {
      LOG_ERROR("invalid tx type");
      return false;
    }

    if (shuffle_outs)
    {
      std::shuffle(destinations.begin(), destinations.end(), crypto::random_device{});
    }

    // sort ins by their key image
    std::vector<size_t> ins_order(sources.size());
    for (size_t n = 0; n < sources.size(); ++n)
      ins_order[n] = n;
    std::sort(ins_order.begin(), ins_order.end(), [&](const size_t i0, const size_t i1) {
      const txin_zephyr_key &tk0 = boost::get<txin_zephyr_key>(tx.vin[i0]);
      const txin_zephyr_key &tk1 = boost::get<txin_zephyr_key>(tx.vin[i1]);
      return memcmp(&tk0.k_image, &tk1.k_image, sizeof(tk0.k_image)) > 0;
    });
    tools::apply_permutation(ins_order, [&] (size_t i0, size_t i1) {
      std::swap(tx.vin[i0], tx.vin[i1]);
      std::swap(in_contexts[i0], in_contexts[i1]);
      std::swap(sources[i0], sources[i1]);
    });

    // figure out if we need to make additional tx pubkeys
    size_t num_stdaddresses = 0;
    size_t num_subaddresses = 0;
    account_public_address single_dest_subaddress;
    classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);

    // if this is a single-destination transfer to a subaddress, we set the tx pubkey to R=s*D
    if (num_stdaddresses == 0 && num_subaddresses == 1)
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultKey(rct::pk2rct(single_dest_subaddress.m_spend_public_key), rct::sk2rct(tx_key)));
    }
    else
    {
      txkey_pub = rct::rct2pk(hwdev.scalarmultBase(rct::sk2rct(tx_key)));
    }
    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_pub_key));
    add_tx_pub_key_to_extra(tx, txkey_pub);

    std::vector<crypto::public_key> additional_tx_public_keys;

    // we don't need to include additional tx keys if:
    //   - all the destinations are standard addresses
    //   - there's only one destination which is a subaddress
    bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
    if (need_additional_txkeys)
      CHECK_AND_ASSERT_MES(destinations.size() == additional_tx_keys.size(), false, "Wrong amount of additional tx keys");

    uint64_t summary_outs_money = 0;
    tx.amount_minted = tx.amount_burnt = 0;

    //fill outputs
    size_t output_index = 0;
    for(const tx_destination_entry& dst_entr: destinations)
    {
      CHECK_AND_ASSERT_MES(dst_entr.dest_amount > 0 || tx.version > 1, false, "Destination with wrong amount: " << dst_entr.dest_amount);
      crypto::public_key out_eph_public_key;
      crypto::view_tag view_tag;

      hwdev.generate_output_ephemeral_keys(tx.version,sender_account_keys, txkey_pub, tx_key,
                                           dst_entr, change_addr, output_index,
                                           need_additional_txkeys, additional_tx_keys,
                                           additional_tx_public_keys, amount_keys, out_eph_public_key,
                                           use_view_tags, view_tag);

      tx_out out;
      cryptonote::set_tx_out(dst_entr.dest_asset_type, dst_entr.dest_amount, out_eph_public_key, use_view_tags, view_tag, out);
      tx.vout.push_back(out);
      output_index++;

      summary_outs_money += dst_entr.amount;

      if (source_asset != dest_asset) {
        if (dst_entr.dest_asset_type == dest_asset) {
          tx.amount_minted += dst_entr.dest_amount;
          tx.amount_burnt += dst_entr.amount;
        }
      }
    }

    if (source_asset != dest_asset) {
      multiprecision::int128_t conversion_this_tx_zeph = 0;
      multiprecision::int128_t conversion_this_tx_stables = 0;
      multiprecision::int128_t conversion_this_tx_reserves = 0;

      if (tx_type == transaction_type::MINT_STABLE) {
        conversion_this_tx_zeph = tx.amount_burnt; // Added to the reserve
        conversion_this_tx_stables = tx.amount_minted;
      } else if (tx_type == transaction_type::REDEEM_STABLE) {
        conversion_this_tx_stables = tx.amount_burnt;
        conversion_this_tx_zeph = tx.amount_minted; // Deducted from the reserve
      } else if (tx_type == transaction_type::MINT_RESERVE) {
        conversion_this_tx_zeph = tx.amount_burnt;
        conversion_this_tx_reserves = tx.amount_minted;
      } else if (tx_type == transaction_type::REDEEM_RESERVE) {
        conversion_this_tx_reserves = tx.amount_burnt;
        conversion_this_tx_zeph = tx.amount_minted;
      } else {
        LOG_ERROR("Invalid transaction type found for conversion transaction");
        return false;
      }

      if (!reserve_ratio_satisfied(circ_amounts, pricing_record_history, pr, tx_type, conversion_this_tx_zeph, conversion_this_tx_stables, conversion_this_tx_reserves, hf_version)) {
        LOG_ERROR("reserve ratio not satisfied");
        return false;
      }
    }

    CHECK_AND_ASSERT_MES(additional_tx_public_keys.size() == additional_tx_keys.size(), false, "Internal error creating additional public keys");

    remove_field_from_tx_extra(tx.extra, typeid(tx_extra_additional_pub_keys));

    LOG_PRINT_L2("tx pubkey: " << txkey_pub);
    if (need_additional_txkeys)
    {
      LOG_PRINT_L2("additional tx pubkeys: ");
      for (size_t i = 0; i < additional_tx_public_keys.size(); ++i)
        LOG_PRINT_L2(additional_tx_public_keys[i]);
      add_additional_tx_pub_keys_to_extra(tx.extra, additional_tx_public_keys);
    }

    if (!sort_tx_extra(tx.extra, tx.extra))
      return false;

    CHECK_AND_ASSERT_MES(tx.extra.size() <= MAX_TX_EXTRA_SIZE, false, "TX extra size (" << tx.extra.size() << ") is greater than max allowed (" << MAX_TX_EXTRA_SIZE << ")");

    //check money
    if(summary_outs_money > summary_inputs_money )
    {
      LOG_ERROR("Transaction inputs money ("<< summary_inputs_money << ") less than outputs money (" << summary_outs_money << ")");
      return false;
    }

    // check for watch only wallet
    bool zero_secret_key = true;
    for (size_t i = 0; i < sizeof(sender_account_keys.m_spend_secret_key); ++i)
      zero_secret_key &= (sender_account_keys.m_spend_secret_key.data[i] == 0);
    if (zero_secret_key)
    {
      MDEBUG("Null secret key, skipping signatures");
    }

    if (tx.version == 1)
    {
      //generate ring signatures
      crypto::hash tx_prefix_hash;
      get_transaction_prefix_hash(tx, tx_prefix_hash);

      std::stringstream ss_ring_s;
      size_t i = 0;
      for(const tx_source_entry& src_entr:  sources)
      {
        ss_ring_s << "pub_keys:" << ENDL;
        std::vector<const crypto::public_key*> keys_ptrs;
        std::vector<crypto::public_key> keys(src_entr.outputs.size());
        size_t ii = 0;
        for(const tx_source_entry::output_entry& o: src_entr.outputs)
        {
          keys[ii] = rct2pk(o.second.dest);
          keys_ptrs.push_back(&keys[ii]);
          ss_ring_s << o.second.dest << ENDL;
          ++ii;
        }

        tx.signatures.push_back(std::vector<crypto::signature>());
        std::vector<crypto::signature>& sigs = tx.signatures.back();
        sigs.resize(src_entr.outputs.size());
        if (!zero_secret_key)
          crypto::generate_ring_signature(tx_prefix_hash, boost::get<txin_zephyr_key>(tx.vin[i]).k_image, keys_ptrs, in_contexts[i].in_ephemeral.sec, src_entr.real_output, sigs.data());
        ss_ring_s << "signatures:" << ENDL;
        std::for_each(sigs.begin(), sigs.end(), [&](const crypto::signature& s){ss_ring_s << s << ENDL;});
        ss_ring_s << "prefix_hash:" << tx_prefix_hash << ENDL << "in_ephemeral_key: " << in_contexts[i].in_ephemeral.sec << ENDL << "real_output: " << src_entr.real_output << ENDL;
        i++;
      }

      MCINFO("construct_tx", "transaction_created: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx) << ENDL << ss_ring_s.str());
    }
    else
    {
      size_t n_total_outs = sources[0].outputs.size(); // only for non-simple rct

      // the non-simple version is slightly smaller, but assumes all real inputs
      // are on the same index, so can only be used if there just one ring.
      bool use_simple_rct = sources.size() > 1 || rct_config.range_proof_type != rct::RangeProofBorromean;

      if (!use_simple_rct)
      {
        // non simple ringct requires all real inputs to be at the same index for all inputs
        for(const tx_source_entry& src_entr:  sources)
        {
          if(src_entr.real_output != sources.begin()->real_output)
          {
            LOG_ERROR("All inputs must have the same index for non-simple ringct");
            return false;
          }
        }

        // enforce same mixin for all outputs
        for (size_t i = 1; i < sources.size(); ++i) {
          if (n_total_outs != sources[i].outputs.size()) {
            LOG_ERROR("Non-simple ringct transaction has varying ring size");
            return false;
          }
        }
      }

      uint64_t amount_in = 0, amount_out = 0;
      rct::ctkeyV inSk;
      inSk.reserve(sources.size());
      // mixRing indexing is done the other way round for simple
      rct::ctkeyM mixRing(use_simple_rct ? sources.size() : n_total_outs);
      rct::keyV destinations;
      std::vector<uint64_t> inamounts, outamounts;
      std::map<size_t, std::string> outamounts_features;
      std::vector<unsigned int> index;
      for (size_t i = 0; i < sources.size(); ++i)
      {
        rct::ctkey ctkey;
        amount_in += sources[i].amount;
        inamounts.push_back(sources[i].amount);
        index.push_back(sources[i].real_output);
        // inSk: (secret key, mask)
        ctkey.dest = rct::sk2rct(in_contexts[i].in_ephemeral.sec);
        ctkey.mask = sources[i].mask;
        inSk.push_back(ctkey);
        memwipe(&ctkey, sizeof(rct::ctkey));
        // inPk: (public key, commitment)
        // will be done when filling in mixRing
      }
      for (size_t i = 0; i < tx.vout.size(); ++i)
      {
        crypto::public_key output_public_key;
        get_output_public_key(tx.vout[i], output_public_key);
        
        std::string output_asset_type;
        bool ok = cryptonote::get_output_asset_type(tx.vout[i], output_asset_type);
        if (!ok) {
          LOG_ERROR("failed to get output asset type for tx.vout[" << i << "]");
          return false;
        }
        
        destinations.push_back(rct::pk2rct(output_public_key));
        outamounts.push_back(tx.vout[i].amount);
        outamounts_features[i] = output_asset_type;
        amount_out += tx.vout[i].amount;
      }

      if (use_simple_rct)
      {
        // mixRing indexing is done the other way round for simple
        for (size_t i = 0; i < sources.size(); ++i)
        {
          mixRing[i].resize(sources[i].outputs.size());
          for (size_t n = 0; n < sources[i].outputs.size(); ++n)
          {
            mixRing[i][n] = sources[i].outputs[n].second;
          }
        }
      }
      else
      {
        for (size_t i = 0; i < n_total_outs; ++i) // same index assumption
        {
          mixRing[i].resize(sources.size());
          for (size_t n = 0; n < sources.size(); ++n)
          {
            mixRing[i][n] = sources[n].outputs[i].second;
          }
        }
      }

      // fee
      uint64_t fee = 0;
      if (!use_simple_rct && amount_in > amount_out)
        outamounts.push_back(amount_in - amount_out);
      else
        fee = summary_inputs_money - summary_outs_money;

      // zero out all amounts to mask rct outputs, real amounts are now encrypted
      for (size_t i = 0; i < tx.vin.size(); ++i)
      {
        if (sources[i].rct)
          boost::get<txin_zephyr_key>(tx.vin[i]).amount = 0;
      }
      for (size_t i = 0; i < tx.vout.size(); ++i)
        tx.vout[i].amount = 0;

      


      crypto::hash tx_prefix_hash;
      get_transaction_prefix_hash(tx, tx_prefix_hash, hwdev);
      rct::ctkeyV outSk;
      if (use_simple_rct)
        tx.rct_signatures = rct::genRctSimple(
          rct::hash2rct(tx_prefix_hash),
          inSk, 
          destinations,
          tx_type,
          source_asset,
          pr,
          inamounts,
          outamounts,
          outamounts_features,
          fee,
          mixRing,
          amount_keys,
          index,
          outSk,
          rct_config,
          hwdev,
          hf_version
        );
      else
        tx.rct_signatures = rct::genRct(rct::hash2rct(tx_prefix_hash), inSk, destinations, outamounts, mixRing, amount_keys, sources[0].real_output, outSk, rct_config, hwdev); // same index assumption
      memwipe(inSk.data(), inSk.size() * sizeof(rct::ctkey));

      CHECK_AND_ASSERT_MES(tx.vout.size() == outSk.size(), false, "outSk size does not match vout");

      MCINFO("construct_tx", "transaction_created: " << get_transaction_hash(tx) << ENDL << obj_to_json_str(tx) << ENDL);
    }

    tx.invalidate_hashes();

    return true;
  }
  //---------------------------------------------------------------
  bool construct_tx_and_get_tx_key(
    const account_keys& sender_account_keys,
    const std::unordered_map<crypto::public_key,
    subaddress_index>& subaddresses,
    std::vector<tx_source_entry>& sources,
    std::vector<tx_destination_entry>& destinations,
    const boost::optional<cryptonote::account_public_address>& change_addr,
    const std::vector<uint8_t> &extra,
    transaction& tx,
    const std::string& source_asset,
    const std::string& dest_asset,
    const uint64_t current_height,
    const uint8_t hf_version,
    const oracle::pricing_record& pr,
    const std::vector<std::pair<std::string, std::string>> circ_amounts,
    const std::vector<oracle::pricing_record>& pricing_record_history,
    uint64_t unlock_time,
    crypto::secret_key &tx_key,
    std::vector<crypto::secret_key> &additional_tx_keys,
    bool rct,
    const rct::RCTConfig &rct_config,
    bool use_view_tags
  ){
    hw::device &hwdev = sender_account_keys.get_device();
    hwdev.open_tx(tx_key);
    try {
      // figure out if we need to make additional tx pubkeys
      size_t num_stdaddresses = 0;
      size_t num_subaddresses = 0;
      account_public_address single_dest_subaddress;
      classify_addresses(destinations, change_addr, num_stdaddresses, num_subaddresses, single_dest_subaddress);
      bool need_additional_txkeys = num_subaddresses > 0 && (num_stdaddresses > 0 || num_subaddresses > 1);
      if (need_additional_txkeys)
      {
        additional_tx_keys.clear();
        for (size_t i = 0; i < destinations.size(); ++i)
        {
          additional_tx_keys.push_back(keypair::generate(sender_account_keys.get_device()).sec);
        }
      }

      bool shuffle_outs = true;
      bool r = construct_tx_with_tx_key(
        sender_account_keys,
        subaddresses,
        sources,
        destinations,
        change_addr,
        extra,
        tx,
        source_asset,
        dest_asset,
        current_height,
        hf_version,
        pr,
        circ_amounts,
        pricing_record_history,
        unlock_time,
        tx_key,
        additional_tx_keys,
        rct,
        rct_config,
        shuffle_outs,
        use_view_tags
      );
      hwdev.close_tx();
      return r;
    } catch(...) {
      hwdev.close_tx();
      throw;
    }
  }
  //---------------------------------------------------------------
  bool construct_tx(const account_keys& sender_account_keys, std::vector<tx_source_entry>& sources, const std::vector<tx_destination_entry>& destinations, const boost::optional<cryptonote::account_public_address>& change_addr, const std::vector<uint8_t> &extra, transaction& tx, uint64_t unlock_time, const uint8_t hf_version)
  {
    std::unordered_map<crypto::public_key, cryptonote::subaddress_index> subaddresses;
    subaddresses[sender_account_keys.m_account_address.m_spend_public_key] = {0,0};
    crypto::secret_key tx_key;
    std::vector<crypto::secret_key> additional_tx_keys;
    std::vector<tx_destination_entry> destinations_copy = destinations;
    
    std::vector<std::pair<std::string, std::string>> circ_supply; // = m_blockchain_storage.get_db().get_circulating_supply();
    std::vector<oracle::pricing_record> pricing_record_history;
    return construct_tx_and_get_tx_key(sender_account_keys, subaddresses, sources, destinations_copy, change_addr, extra, tx, "ZEPH", "ZEPH", 100, hf_version, oracle::pricing_record(), circ_supply, pricing_record_history, unlock_time, tx_key, additional_tx_keys, false, { rct::RangeProofBorromean, 0});
  }
  //---------------------------------------------------------------
  bool generate_genesis_block(
      block& bl
    , std::string const & genesis_tx
    , uint32_t nonce
    )
  {
    //genesis block
    bl = {};

    blobdata tx_bl;
    bool r = string_tools::parse_hexstr_to_binbuff(genesis_tx, tx_bl);
    CHECK_AND_ASSERT_MES(r, false, "GENESIS: failed to parse coinbase tx from hard coded blob");
    r = parse_and_validate_tx_from_blob(tx_bl, bl.miner_tx);
    CHECK_AND_ASSERT_MES(r, false, "GENESIS 2: failed to parse coinbase tx from hard coded blob");
    bl.major_version = CURRENT_BLOCK_MAJOR_VERSION;
    bl.minor_version = CURRENT_BLOCK_MINOR_VERSION;
    bl.timestamp = 0;
    bl.nonce = nonce;
    miner::find_nonce_for_given_block([](const cryptonote::block &b, uint64_t height, const crypto::hash *seed_hash, unsigned int threads, crypto::hash &hash){
      return cryptonote::get_block_longhash(NULL, b, hash, height, seed_hash, threads);
    }, bl, 1, 0, NULL);
    bl.invalidate_hashes();
    return true;
  }
  //---------------------------------------------------------------
  void get_altblock_longhash(const block& b, crypto::hash& res, const crypto::hash& seed_hash)
  {
    blobdata bd = get_block_hashing_blob(b);
    rx_slow_hash(seed_hash.data, bd.data(), bd.size(), res.data);
  }

  bool get_block_longhash(const Blockchain *pbc, const blobdata& bd, crypto::hash& res, const uint64_t height, const int major_version, const crypto::hash *seed_hash, const int miners)
  {
    crypto::hash hash;
    if (pbc != NULL)
    {
      const uint64_t seed_height = rx_seedheight(height);
      hash = seed_hash ? *seed_hash : pbc->get_pending_block_id_by_height(seed_height);
    } else
    {
      memset(&hash, 0, sizeof(hash));  // only happens when generating genesis block
    }
    rx_slow_hash(hash.data, bd.data(), bd.size(), res.data);

    return true;
  }

  bool get_block_longhash(const Blockchain *pbc, const block& b, crypto::hash& res, const uint64_t height, const crypto::hash *seed_hash, const int miners)
  {
    blobdata bd = get_block_hashing_blob(b);
	return get_block_longhash(pbc, bd, res, height, b.major_version, seed_hash, miners);
  }

  crypto::hash get_block_longhash(const Blockchain *pbc, const block& b, const uint64_t height, const crypto::hash *seed_hash, const int miners)
  {
    crypto::hash p = crypto::null_hash;
    get_block_longhash(pbc, b, p, height, seed_hash, miners);
    return p;
  }
}
