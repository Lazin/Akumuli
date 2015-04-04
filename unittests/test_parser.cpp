#include <iostream>

#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE Main
#include <boost/test/unit_test.hpp>

#include "seriesparser.h"
#include "datetime.h"

using namespace Akumuli;
using namespace Akumuli::QP;

void logger(int errlvl, const char* msg) {
    if (errlvl == AKU_LOG_ERROR) {
        std::cout << msg << std::endl;
    }
}

BOOST_AUTO_TEST_CASE(Test_stringpool_0) {

    StringPool pool;
    const char* foo = "foo";
    auto result_foo = pool.add(foo, foo + 3, 0ul);
    const char* bar = "123456";
    auto result_bar = pool.add(bar, bar + 6, 0ul);
    BOOST_REQUIRE_EQUAL(result_foo.second, 3);
    BOOST_REQUIRE_EQUAL(std::string(result_foo.first, result_foo.first + result_foo.second), foo);
    BOOST_REQUIRE_EQUAL(result_bar.second, 6);
    BOOST_REQUIRE_EQUAL(std::string(result_bar.first, result_bar.first + result_bar.second), bar);
}

BOOST_AUTO_TEST_CASE(Test_seriesmatcher_0) {

    SeriesMatcher matcher(1ul);
    const char* foo = "foobar";
    const char* bar = "barfoobar";
    const char* buz = "buz";
    matcher.add(foo, foo+6);
    matcher.add(bar, bar+9);

    auto foo_id = matcher.match(foo, foo+6);
    BOOST_REQUIRE_EQUAL(foo_id, 1ul);

    auto bar_id = matcher.match(bar, bar+9);
    BOOST_REQUIRE_EQUAL(bar_id, 2ul);

    auto buz_id = matcher.match(buz, buz+3);
    BOOST_REQUIRE_EQUAL(buz_id, 0ul);
}

BOOST_AUTO_TEST_CASE(Test_seriesparser_0) {

    const char* series = " cpu  region=europe   host=127.0.0.1 ";
    auto len = strlen(series);
    char out[40];
    const char* pbegin = nullptr;
    const char* pend = nullptr;
    int status = SeriesParser::to_normal_form(series, series + len, out, out + len, &pbegin, &pend);

    BOOST_REQUIRE_EQUAL(status, AKU_SUCCESS);

    std::string expected = "cpu host=127.0.0.1 region=europe";
    std::string actual = std::string((const char*)out, pend);
    BOOST_REQUIRE_EQUAL(expected, actual);

    std::string keystr = std::string(pbegin, pend);
    BOOST_REQUIRE_EQUAL("host=127.0.0.1 region=europe", keystr);
}

BOOST_AUTO_TEST_CASE(Test_seriesparser_1) {

    const char* series = "cpu";
    auto len = strlen(series);
    char out[27];
    const char* pend = nullptr;
    int status = SeriesParser::to_normal_form(series, series + len, out, out + len, &pend, &pend);
    BOOST_REQUIRE_EQUAL(status, AKU_EBAD_DATA);
}

BOOST_AUTO_TEST_CASE(Test_seriesparser_2) {

    const char* series = "cpu region host=127.0.0.1 ";
    auto len = strlen(series);
    char out[27];
    const char* pend = nullptr;
    int status = SeriesParser::to_normal_form(series, series + len, out, out + len, &pend, &pend);
    BOOST_REQUIRE_EQUAL(status, AKU_EBAD_DATA);
}

BOOST_AUTO_TEST_CASE(Test_seriesparser_3) {

    const char* series = "cpu region=europe host";
    auto len = strlen(series);
    char out[27];
    const char* pend = nullptr;
    int status = SeriesParser::to_normal_form(series, series + len, out, out + len, &pend, &pend);
    BOOST_REQUIRE_EQUAL(status, AKU_EBAD_DATA);
}

BOOST_AUTO_TEST_CASE(Test_seriesparser_4) {

    auto len = AKU_LIMITS_MAX_SNAME + 1;
    char series[len];
    char out[len];
    const char* pend = nullptr;
    int status = SeriesParser::to_normal_form(series, series + len, out, out + len, &pend, &pend);
    BOOST_REQUIRE_EQUAL(status, AKU_EBAD_DATA);
}

BOOST_AUTO_TEST_CASE(Test_seriesparser_5) {

    auto len = AKU_LIMITS_MAX_SNAME - 1;
    char series[len];
    char out[10];
    const char* pend = nullptr;
    int status = SeriesParser::to_normal_form(series, series + len, out, out + 10, &pend, &pend);
    BOOST_REQUIRE_EQUAL(status, AKU_EBAD_ARG);
}

// Test queryprocessor building

BOOST_AUTO_TEST_CASE(Test_queryprocessor_building_0) {

    SeriesMatcher matcher(1ul);
    const char* json =
        " {                                        "
        "      \"sample\": { \"reservoir\": 1000 },"
        "      \"metric\": \"cpu\",                "
        "      \"range\" : {                       "
        "           \"from\": \"20150101T000000\", "
        "           \"to\"  : \"20150102T000000\"  "
        "       },                                 "
        "      \"where\": [                        "
        "                      {\"in\":            "
        "                          {\"key3\":      "
        "                                 [1, 2, 3]"
        "                          }               "
        "                      }                   "
        "                 ]                        "
        " }                                        ";
    auto qproc = matcher.build_query_processor(json, &logger);
    BOOST_REQUIRE(qproc->root_node->get_type() == Node::RandomSampler);
    BOOST_REQUIRE(qproc->metrics.size() == 1);
    BOOST_REQUIRE(qproc->metrics.at(0) == "cpu");
}

BOOST_AUTO_TEST_CASE(Test_queryprocessor_building_1) {

    SeriesMatcher matcher(1ul);
    const char* json =
        " {                                        "
        "      \"sample\": { \"reservoir\": 1000 },"
        "      \"metric\": [\"cpu\", \"mem\"],     "
        "      \"range\" : {                       "
        "           \"from\": \"20150101T000000\", "
        "           \"to\"  : \"20150102T000000\"  "
        "       },                                 "
        "      \"where\": [                        "
        "                      {\"in\":            "
        "                          {\"key3\":      "
        "                                 [1, 2, 3]"
        "                          }               "
        "                      }                   "
        "                 ]                        "
        " }                                        ";
    auto qproc = matcher.build_query_processor(json, &logger);
    BOOST_REQUIRE(qproc->root_node->get_type() == Node::RandomSampler);
    BOOST_REQUIRE(qproc->metrics.size() == 2);
    auto m1 = qproc->metrics.at(0);
    auto m2 = qproc->metrics.at(1);
    if (m1 == "cpu") {
        BOOST_REQUIRE(m2 == "mem");
    } else {
        BOOST_REQUIRE(m1 == "mem");
        BOOST_REQUIRE(m2 == "cpu");
    }
    auto first_ts  = boost::posix_time::ptime(boost::gregorian::date(2015, 01, 01));
    auto second_ts = boost::posix_time::ptime(boost::gregorian::date(2015, 01, 02));
    BOOST_REQUIRE(qproc->lowerbound == DateTimeUtil::from_boost_ptime(first_ts));
    BOOST_REQUIRE(qproc->upperbound == DateTimeUtil::from_boost_ptime(second_ts));
}
