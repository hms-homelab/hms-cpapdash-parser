#ifdef CPAPDASH_WITH_SEFAM

// The Sefam INI is the whole reason this format is cheap to support: it declares
// its own channel table, so the parser hard-codes none of it. That makes the
// reader load-bearing -- a misread Freq or Bit silently rescales a therapy
// number -- so it is tested on its own here.

#include <gtest/gtest.h>

#include "cpapdash/parser/SefamParser.h"

#include <cmath>
#include <string>

using namespace cpapdash::parser;

namespace {

std::string minimalIni() {
    return
        "[Create Info]\r\n"
        "Created By=S.Box_AUTO\r\n"
        "Serial Number=1263R24462543\r\n"
        "Version=VER :A020400\r\n"
        "Date=13/12/25 23:45:50\r\n"
        "[Oximeter]\r\n"
        "TYPE=NONE\r\n"
        "[Start Record]\r\n"
        "Hour=23\r\nMin=45\r\nSec=50\r\n"
        "Day=13\r\nMonth=12\r\nYear=2025\r\n"
        "Programmed Record Duration=28800\r\n"
        "Real Record Duration=7200\r\n"
        "[Chan0]\r\n"
        "Name=FLW\r\nDescription=NO\r\nType=4\r\nUnit=lpm\r\n"
        "Min=-180\r\nMax=280\r\nFreq=25\r\nBit=8\r\n";
}

} // namespace

TEST(SefamIni, ReadsIdentityAndChannels) {
    const SefamIni ini = SefamParser::parseIniText(minimalIni());

    ASSERT_TRUE(ini.valid);
    EXPECT_EQ(ini.created_by, "S.Box_AUTO");
    EXPECT_EQ(ini.serial_number, "1263R24462543");
    EXPECT_EQ(ini.oximeter_type, "NONE");
    EXPECT_EQ(ini.programmed_duration_s, 28800);
    EXPECT_EQ(ini.real_duration_s, 7200);

    ASSERT_EQ(ini.channels.size(), 1u);
    EXPECT_EQ(ini.channels[0].name, "FLW");
    EXPECT_EQ(ini.channels[0].unit, "lpm");
    EXPECT_EQ(ini.channels[0].freq, 25);
    EXPECT_EQ(ini.channels[0].bits, 8);
    EXPECT_DOUBLE_EQ(ini.channels[0].min, -180);
    EXPECT_DOUBLE_EQ(ini.channels[0].max, 280);
}

// The firmware string is "VER :A020400". A reader that split on punctuation
// rather than on the first '=' would lose half of it.
TEST(SefamIni, ValueKeepsItsOwnPunctuation) {
    const SefamIni ini = SefamParser::parseIniText(minimalIni());
    EXPECT_EQ(ini.firmware, "VER :A020400");
}

TEST(SefamIni, FindIsCaseInsensitive) {
    const SefamIni ini = SefamParser::parseIniText(minimalIni());
    ASSERT_NE(ini.find("flw"), nullptr);
    ASSERT_NE(ini.find("FLW"), nullptr);
    EXPECT_EQ(ini.find("PRE"), nullptr);
}

TEST(SefamIni, ChannelsComeBackInDeclaredOrder) {
    std::string text = minimalIni();
    text +=
        "[Chan2]\r\nName=LK\r\nUnit=lpm\r\nMin=0\r\nMax=153\r\nFreq=1\r\nBit=8\r\n"
        "[Chan1]\r\nName=PRE\r\nUnit=cmH2O\r\nMin=0\r\nMax=25.5\r\nFreq=5\r\nBit=8\r\n";

    const SefamIni ini = SefamParser::parseIniText(text);
    ASSERT_EQ(ini.channels.size(), 3u);
    EXPECT_EQ(ini.channels[0].name, "FLW");
    EXPECT_EQ(ini.channels[1].name, "PRE");
    EXPECT_EQ(ini.channels[2].name, "LK");
}

TEST(SefamIni, StartRecordGivesTheRecordingStart) {
    const SefamIni ini = SefamParser::parseIniText(minimalIni());
    ASSERT_TRUE(ini.start.has_value());

    const std::time_t t = std::chrono::system_clock::to_time_t(*ini.start);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    EXPECT_EQ(local.tm_year + 1900, 2025);
    EXPECT_EQ(local.tm_mon + 1, 12);
    EXPECT_EQ(local.tm_mday, 13);
    EXPECT_EQ(local.tm_hour, 23);
    EXPECT_EQ(local.tm_min, 45);
    EXPECT_EQ(local.tm_sec, 50);
}

// [Create Info] writes the date as one DD/MM/YY string; [Start Record] writes
// the same instant as six separate numbers. Where both are present the six
// numbers win, because they leave nothing to interpret.
TEST(SefamIni, StartRecordBeatsTheCreateInfoDate) {
    std::string text =
        "[Create Info]\r\nDate=01/01/20 01:02:03\r\n"
        "[Start Record]\r\nHour=23\r\nMin=45\r\nSec=50\r\n"
        "Day=13\r\nMonth=12\r\nYear=2025\r\n"
        "[Chan0]\r\nName=FLW\r\nMin=0\r\nMax=255\r\nFreq=25\r\nBit=8\r\n";

    const SefamIni ini = SefamParser::parseIniText(text);
    ASSERT_TRUE(ini.start.has_value());

    const std::time_t t = std::chrono::system_clock::to_time_t(*ini.start);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    EXPECT_EQ(local.tm_year + 1900, 2025);
    EXPECT_EQ(local.tm_mday, 13);
}

// With no [Start Record] the DD/MM/YY string is all there is, and it is day
// first: the vendor's own software displays dates that way.
TEST(SefamIni, CreateInfoDateIsDayFirstAndTwoDigitYearsAreWindowed) {
    std::string text =
        "[Create Info]\r\nDate=13/12/25 23:45:50\r\n"
        "[Chan0]\r\nName=FLW\r\nMin=0\r\nMax=255\r\nFreq=25\r\nBit=8\r\n";

    const SefamIni ini = SefamParser::parseIniText(text);
    ASSERT_TRUE(ini.start.has_value());

    const std::time_t t = std::chrono::system_clock::to_time_t(*ini.start);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    EXPECT_EQ(local.tm_year + 1900, 2025);
    EXPECT_EQ(local.tm_mon + 1, 12);
    EXPECT_EQ(local.tm_mday, 13);
}

TEST(SefamIni, LeadingBomDoesNotSwallowTheFirstSection) {
    const std::string text = "\xEF\xBB\xBF" + minimalIni();
    const SefamIni ini = SefamParser::parseIniText(text);
    ASSERT_TRUE(ini.valid);
    EXPECT_EQ(ini.created_by, "S.Box_AUTO");
}

TEST(SefamIni, UnixLineEndingsWorkToo) {
    std::string text = minimalIni();
    std::string lf;
    for (char c : text) if (c != '\r') lf.push_back(c);

    const SefamIni ini = SefamParser::parseIniText(lf);
    ASSERT_TRUE(ini.valid);
    EXPECT_EQ(ini.channels.size(), 1u);
}

// The INI arrives from a user's SD card, so every numeric field is untrusted.
// cpapdash-api parses on a detached thread where an escaping exception is a
// std::terminate of the service, not a failed parse.
TEST(SefamIni, MalformedNumbersDegradeInsteadOfThrowing) {
    const std::string text =
        "[Start Record]\r\nHour=x\r\nMin=y\r\nSec=z\r\n"
        "Day=99999999999999999999\r\nMonth=nope\r\nYear=\r\n"
        "Real Record Duration=lots\r\n"
        "[Chan0]\r\nName=FLW\r\nMin=low\r\nMax=high\r\nFreq=fast\r\nBit=eight\r\n";

    SefamIni ini;
    EXPECT_NO_THROW({ ini = SefamParser::parseIniText(text); });

    ASSERT_EQ(ini.channels.size(), 1u);
    EXPECT_EQ(ini.channels[0].freq, 0);
    EXPECT_EQ(ini.channels[0].bits, 8);
    EXPECT_DOUBLE_EQ(ini.channels[0].min, 0);
    EXPECT_DOUBLE_EQ(ini.channels[0].max, 0);
    EXPECT_EQ(ini.real_duration_s, 0);
    EXPECT_FALSE(ini.start.has_value());
}

TEST(SefamIni, NoChannelsMeansNothingToRead) {
    const std::string text =
        "[Create Info]\r\nCreated By=S.Box_AUTO\r\n"
        "[Start Record]\r\nHour=1\r\nMin=2\r\nSec=3\r\nDay=4\r\nMonth=5\r\nYear=2026\r\n";
    EXPECT_FALSE(SefamParser::parseIniText(text).valid);
}

TEST(SefamIni, ANamelessChannelIsNotAChannel) {
    const std::string text =
        "[Chan0]\r\nUnit=lpm\r\nFreq=25\r\nBit=8\r\n"
        "[Chan1]\r\nName=PRE\r\nUnit=cmH2O\r\nMin=0\r\nMax=25.5\r\nFreq=5\r\nBit=8\r\n";

    const SefamIni ini = SefamParser::parseIniText(text);
    ASSERT_EQ(ini.channels.size(), 1u);
    EXPECT_EQ(ini.channels[0].name, "PRE");
}

TEST(SefamIni, SectionsThatOnlyLookLikeChannelsAreIgnored) {
    const std::string text =
        "[Channel0]\r\nName=NOPE\r\nFreq=1\r\nBit=8\r\n"
        "[Chan]\r\nName=ALSONOPE\r\nFreq=1\r\nBit=8\r\n"
        "[Chan0]\r\nName=FLW\r\nMin=0\r\nMax=255\r\nFreq=25\r\nBit=8\r\n";

    const SefamIni ini = SefamParser::parseIniText(text);
    ASSERT_EQ(ini.channels.size(), 1u);
    EXPECT_EQ(ini.channels[0].name, "FLW");
}

// The INI's Type= is a per-quantity kind code, separate from the name: the donor
// card writes 4 for flow and leak, 11 for pressure, 13 for the unitless
// channels, 16 for SpO2 and 17 for heart rate. Carried in case it turns out to
// be a better key than the name.
TEST(SefamIni, ChannelTypeIsCarried) {
    const std::string text =
        "[Chan0]\r\nName=FLW\r\nType=4\r\nUnit=lpm\r\nMin=-180\r\nMax=280\r\n"
        "Freq=25\r\nBit=8\r\n"
        "[Chan1]\r\nName=HRT\r\nType=17\r\nUnit=bpm\r\nMin=25\r\nMax=280\r\n"
        "Freq=1\r\nBit=8\r\n";

    const SefamIni ini = SefamParser::parseIniText(text);
    ASSERT_EQ(ini.channels.size(), 2u);
    EXPECT_EQ(ini.channels[0].type, 4);
    EXPECT_EQ(ini.channels[1].type, 17);
}

TEST(SefamIni, BytesPerSampleFollowsTheDeclaredBitDepth) {
    SefamChannel eight, sixteen;
    eight.bits = 8;
    sixteen.bits = 16;
    EXPECT_EQ(eight.bytesPerSample(), 1);
    EXPECT_EQ(sixteen.bytesPerSample(), 2);
}

#endif // CPAPDASH_WITH_SEFAM
