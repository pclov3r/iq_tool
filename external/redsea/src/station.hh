/*
 * Copyright (c) Oona Räisänen
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */
#ifndef STATION_HH_
#define STATION_HH_

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

#include "src/group.hh"
#include "src/options.hh"
#include "src/rft.hh"
#include "src/text/radiotext.hh"
#include "src/text/rdsstring.hh"
#include "src/tmc/tmc.hh"
#include "src/util/tree.hh"
#include "src/util/util.hh"

namespace redsea {

class ObjectTree;

class Station {
 public:
  Station() = delete;
  Station(const Options& options, int which_channel, std::uint16_t pi);
  Station(const Options& options, int which_channel);
  void updateAndPrint(const Group& group, std::ostream& stream);
  std::uint16_t getPI() const;

  // --- ADDED: Getters for C wrapper ---
  std::string getPS() const { return ps_.text.getLastCompleteString(); }
  std::string getRT() const { return radiotext_.text.getLastCompleteString(); }
  std::string getPTYN() const { return ptyname_.text.getLastCompleteString(); }
  std::string getRawPS() const { return ps_.text.str(); }
  std::string getRawRT() const { return radiotext_.text.str(); }
  std::string getClockTime() const { return clock_time_; }

  std::vector<int> getAltFreqs() const { return alt_freq_list_.getRawList(); }

  std::uint16_t getECC() const { return ecc_; }
  std::uint16_t getCC() const { return cc_; }

  int getPTY() const { return pty_; }
  bool getTP() const { return tp_; }
  bool getTA() const { return ta_; }
  bool getMusic() const { return music_; }

  bool getStereo() const { return di_stereo_; }
  bool getCompressed() const { return di_compressed_; }
  bool getDynamicPTY() const { return di_dynamic_pty_; }
  bool getArtificialHead() const { return di_artificial_head_; }

 private:
  void decodeBasics(const Group& group, ObjectTree& out);
  void decodeType0(const Group& group, ObjectTree& out);
  void decodeType1(const Group& group, ObjectTree& out);
  void decodeType2(const Group& group, ObjectTree& out);
  void decodeType3A(const Group& group, ObjectTree& out);
  void decodeType4A(const Group& group, ObjectTree& out);
  void decodeType5(const Group& group, ObjectTree& out);
  void decodeType6(const Group& group, ObjectTree& out);
  void decodeType7A(const Group& group, ObjectTree& out);
  void decodeType9A(const Group& group, ObjectTree& out);
  void decodeType10A(const Group& group, ObjectTree& out);
  void decodeType14(const Group& group, ObjectTree& out);
  void decodeType15A(const Group& group, ObjectTree& out);
  void decodeType15B(const Group& group, ObjectTree& out);
  void decodeODAGroup(const Group& group, ObjectTree& out);
  void decodeC(const Group& group, ObjectTree& out);
  void parseEnhancedRT(const Group& group, ObjectTree& out);
  void parseDAB(const Group& group, ObjectTree& out);

  std::uint16_t pi_{};
  bool has_pi_{false};
  
  // --- ADDED: Storage for flags ---
  int pty_ = 0;
  bool tp_ = false;
  bool ta_ = false;
  bool music_ = false;
  bool di_stereo_ = false;
  bool di_compressed_ = false;
  bool di_dynamic_pty_ = false;
  bool di_artificial_head_ = false;
  // --------------------------------

  Options options_;
  int which_channel_{};
  ProgramServiceName ps_;
  LongPS long_ps_;
  RadioText radiotext_;
  RadioText ert_;
  PTYName ptyname_;
  RDSString full_tdc_{32U * 4U};
  std::uint16_t pin_{};
  std::uint16_t ecc_{};
  std::uint16_t cc_{};
  int tmc_id_{};
  bool linkage_la_{};
  std::string clock_time_;
  bool has_country_{false};
  std::map<GroupType, std::uint16_t> oda_app_for_group_;
  std::map<std::uint16_t, std::uint16_t> oda_app_for_pipe_;
  bool ert_uses_chartable_e3_{false};
  std::map<std::uint16_t, RDSString> eon_ps_names_;
  std::map<std::uint16_t, AltFreqList> eon_alt_freqs_;
  bool last_group_had_pi_{false};
  AltFreqList alt_freq_list_;

  tmc::TMCService tmc_;

  // One RFT file per pipe
  std::array<RFTFile, 16> rft_file_;
};

void parseRadioTextPlus(const Group& group, RadioText& rt, ObjectTree& out);

}  // namespace redsea
#endif  // STATION_HH_
