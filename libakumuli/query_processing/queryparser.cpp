#include "queryparser.h"
#include "log_iface.h"

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <set>
#include <regex>

#include "datetime.h"
#include "query_processing/limiter.h"

namespace Akumuli {
namespace QP {

SeriesRetreiver::SeriesRetreiver()
{
}

//! Matches all series from one metric
SeriesRetreiver::SeriesRetreiver(std::vector<std::string> const& metric)
    : metric_(metric)
{
}

//! Add tag-name and tag-value pair
aku_Status SeriesRetreiver::add_tag(std::string name, std::string value) {
    if (metric_.empty()) {
        Logger::msg(AKU_LOG_ERROR, "Metric not set");
        return AKU_EBAD_ARG;
    }
    if (tags_.count(name)) {
        // Duplicates not allowed
        Logger::msg(AKU_LOG_ERROR, "Duplicate tag '" + name + "' found");
        return AKU_EBAD_ARG;
    }
    tags_[name] = { value };
    return AKU_SUCCESS;
}

//! Add tag name and set of possible values
aku_Status SeriesRetreiver::add_tags(std::string name, std::vector<std::string> values) {
    if (metric_.empty()) {
        Logger::msg(AKU_LOG_ERROR, "Metric not set");
        return AKU_EBAD_ARG;
    }
    if (tags_.count(name)) {
        // Duplicates not allowed
        Logger::msg(AKU_LOG_ERROR, "Duplicate tag '" + name + "' found");
        return AKU_EBAD_ARG;
    }
    tags_[name] = std::move(values);
    return AKU_SUCCESS;
}

std::tuple<aku_Status, std::vector<aku_ParamId>> SeriesRetreiver::extract_ids(SeriesMatcher const& matcher) const {
    std::vector<aku_ParamId> ids;
    // Three cases, no metric (get all ids), only metric is set and both metric and tags are set.
    if (metric_.empty()) {
        // Case 1, metric not set.
        ids = matcher.get_all_ids();
    } else {
        auto first_metric = metric_.front();
        if (tags_.empty()) {
            // Case 2, only metric is set
            std::stringstream regex;
            regex << first_metric << "(?:\\s[\\w\\.\\-]+=[\\w\\.\\-]+)*";
            std::string expression = regex.str();
            auto results = matcher.regex_match(expression.c_str());
            for (auto res: results) {
                ids.push_back(std::get<2>(res));
            }
        } else {
            // Case 3, both metric and tags are set
            std::stringstream regexp;
            regexp << first_metric;
            for (auto kv: tags_) {
                auto const& key = kv.first;
                bool first = true;
                regexp << "(?:";
                for (auto const& val: kv.second) {
                    if (first) {
                        first = false;
                    } else {
                        regexp << "|";
                    }
                    regexp << "(?:\\s[\\w\\.\\-]+=[\\w\\.\\-]+)*\\s" << key << "=" << val << "(?:\\s[\\w\\.\\-]+=[\\w\\.\\-]+)*";
                }
                regexp << ")";
            }
            std::string expression = regexp.str();
            auto results = matcher.regex_match(expression.c_str());
            for (auto res: results) {
                ids.push_back(std::get<2>(res));
            }
        }

        if (metric_.size() > 1) {
            std::vector<std::string> tail(metric_.begin() + 1, metric_.end());
            std::vector<aku_ParamId> full(ids);
            for (auto metric: tail) {
                for (auto id: ids) {
                    SeriesMatcher::StringT name = matcher.id2str(id);
                    if (name.second == 0) {
                        // This shouldn't happen but it can happen after memory corruption or data-race.
                        // Clearly indicates an error.
                        Logger::msg(AKU_LOG_ERROR, "Matcher data is broken, can read series name for " + std::to_string(id));
                        AKU_PANIC("Matcher data is broken");
                    }
                    std::string series_tags(name.first + first_metric.size(), name.first + name.second);
                    std::string alt_name = metric + series_tags;
                    auto sid = matcher.match(alt_name.data(), alt_name.data() + alt_name.size());
                    full.push_back(sid);  // NOTE: sid (secondary id) can be = 0. This means that there is no such
                                          // combination of metric and tags. Different strategies can be used to deal with
                                          // such cases. Query can leave this element of the tuple blank or discard it.
                }
            }
            ids.swap(full);
        }
    }
    return std::make_tuple(AKU_SUCCESS, ids);
}


static const std::set<std::string> META_QUERIES = {
    "meta:names"
};

bool is_meta_query(std::string name) {
    for (auto perf: META_QUERIES) {
        if (boost::starts_with(name, perf)) {
            return true;
        }
    }
    return false;
}

static std::tuple<aku_Status, std::string> parse_select_stmt(boost::property_tree::ptree const& ptree) {
    auto select = ptree.get_child_optional("select");
    if (select && select->empty()) {
        // select query
        auto str = select->get_value<std::string>("");
        return std::make_tuple(AKU_SUCCESS, str);
    }
    return std::make_tuple(AKU_EQUERY_PARSING_ERROR, "");
}

/** Parse `join` statement, format:
  * { "join": [ "metric1", "metric2", ... ], ... }
  */
static std::tuple<aku_Status, std::vector<std::string>> parse_join_stmt(boost::property_tree::ptree const& ptree) {
    auto join = ptree.get_child_optional("join");
    // value is a list of metric names in proper order
    std::vector<std::string> result;
    if (join) {
        for (auto item: *join) {
            auto value = item.second.get_value_optional<std::string>();
            if (value) {
                result.push_back(*value);
            } else {
                return std::make_tuple(AKU_EQUERY_PARSING_ERROR, result);
            }
        }
    }
    if (result.empty()) {
        return std::make_tuple(AKU_EQUERY_PARSING_ERROR, result);
    }
    return std::make_tuple(AKU_SUCCESS, result);
}

/** Parse `aggregate` statement, format:
  * { "aggregate": { "metric": "func" }, ... }
  */
static std::tuple<aku_Status, std::string, std::string> parse_aggregate_stmt(boost::property_tree::ptree const& ptree) {
    auto aggregate = ptree.get_child_optional("aggregate");
    if (aggregate) {
        // select query
        for (auto kv: *aggregate) {
            auto metric_name = kv.first;
            auto func = kv.second.get_value<std::string>("cnt");
            // Note: only one key-value is parsed at this time, this can be extended to tuples in the future
            return std::make_tuple(AKU_SUCCESS, metric_name, func);
        }
    }
    return std::make_tuple(AKU_EQUERY_PARSING_ERROR, "", "");
}

/**
 * Result of the group-aggregate stmt parsing
 */
struct GroupAggregate {
    std::string metric;
    std::vector<AggregationFunction> func;
    aku_Duration step;
};

/** Parse `group-aggregate` statement, format:
  * { "group-aggregate": { "step": "30s", "metric": "name", "func": ["cnt", "avg"] }, ... }
  * @return status, metric name, functions array, step (as timestamp)
  */
static std::tuple<aku_Status, GroupAggregate> parse_group_aggregate_stmt(boost::property_tree::ptree const& ptree) {
    bool components[] = {
        false, false, false
    };
    GroupAggregate result;
    auto aggregate = ptree.get_child_optional("group-aggregate");
    if (aggregate) {
        // select query
        for (auto kv: *aggregate) {
            auto tag_name = kv.first;
            auto value = kv.second.get_value_optional<std::string>();
            if (tag_name == "step") {
                if (components[0]) {
                    // Duplicate "step" tag
                    Logger::msg(AKU_LOG_ERROR, "Duplicate `step` tag in `group-aggregate` statement");
                    break;
                } else {
                    if (!value) {
                        Logger::msg(AKU_LOG_ERROR, "Tag `step` is not set in `group-aggregate` statement");
                        break;
                    }
                    try {
                        aku_Duration step = DateTimeUtil::parse_duration(value.get().data(), value.get().size());
                        result.step = step;
                        components[0] = true;
                    } catch (const BadDateTimeFormat& e) {
                        Logger::msg(AKU_LOG_ERROR, "Can't parse time-duration: " + *value);
                        Logger::msg(AKU_LOG_ERROR, boost::current_exception_diagnostic_information());
                    }
                }
            } else if (tag_name == "metric") {
                if (!value) {
                    Logger::msg(AKU_LOG_ERROR, "Tag `metric` is not set in `group-aggregate` statement");
                    break;
                }
                if (components[1]) {
                    // Duplicate "metric" tag
                    Logger::msg(AKU_LOG_ERROR, "Duplicate `metric` tag in `group-aggregate` statement");
                    break;
                } else {
                    result.metric = value.get();
                    components[1] = true;
                }
            } else if (tag_name == "func") {
                if (components[2]) {
                    // Duplicate "func" tag
                    Logger::msg(AKU_LOG_ERROR, "Duplicate `func` tag in `group-aggregate` statement");
                    break;
                } else {
                    int n = 0;
                    bool error = false;
                    for (auto child: aggregate->get_child("func")) {
                        auto fn = child.second.get_value_optional<std::string>();
                        if (fn) {
                            aku_Status status;
                            AggregationFunction func;
                            std::tie(status, func) = Aggregation::from_string(*fn);
                            if (status == AKU_SUCCESS) {
                                result.func.push_back(func);
                                n++;
                            } else {
                                Logger::msg(AKU_LOG_ERROR, "Invalid aggregation function `" + fn.get() + "`");
                                error = true;
                                break;
                            }
                        }
                    }
                    if (error) {
                        break;
                    }
                    components[2] = n;
                }
            }
        }
    }
    bool complete = components[0] && components[1] && components[2];
    if (complete) {
        return std::make_tuple(AKU_SUCCESS, result);
    } else if (components[0] == false) {
        Logger::msg(AKU_LOG_ERROR, "Can't validate `group-aggregate` statement, `step` field required");
    } else if (components[1] == false) {
        Logger::msg(AKU_LOG_ERROR, "Can't validate `group-aggregate` statement, `metric` field required");
    } else if (components[2] == false) {
        Logger::msg(AKU_LOG_ERROR, "Can't validate `group-aggregate` statement, `func` field required");
    }
    return std::make_tuple(AKU_EQUERY_PARSING_ERROR, result);
}

/** Parse `oreder-by` statement, format:
  * { "oreder-by": "series", ... }
  */
static std::tuple<aku_Status, OrderBy> parse_orderby(boost::property_tree::ptree const& ptree) {
    auto orderby = ptree.get_child_optional("order-by");
    if (orderby) {
        auto stringval = orderby->get_value<std::string>();
        if (stringval == "time") {
            return std::make_tuple(AKU_SUCCESS, OrderBy::TIME);
        } else if (stringval == "series") {
            return std::make_tuple(AKU_SUCCESS, OrderBy::SERIES);
        } else {
            Logger::msg(AKU_LOG_ERROR, "Invalid 'order-by' statement");
            return std::make_tuple(AKU_EQUERY_PARSING_ERROR, OrderBy::TIME);
        }
    }
    // Default is order by time
    return std::make_tuple(AKU_SUCCESS, OrderBy::TIME);
}

/** Parse `group-by` statement, format:
 *  { ..., "group-by": [ "tag1", "tag2" ] }
 *  or
 *  { ..., "group-by": "tag1" }
 */
static std::tuple<aku_Status, std::vector<std::string>> parse_groupby(boost::property_tree::ptree const& ptree) {
    std::vector<std::string> tags;
    auto groupby = ptree.get_child_optional("group-by");
    if (groupby) {
        for (auto item: *groupby) {
            auto val = item.second.get_value_optional<std::string>();
            if (val) {
                tags.push_back(*val);
            } else {
                return std::make_tuple(AKU_EQUERY_PARSING_ERROR, tags);
            }
        }
    }
    return std::make_tuple(AKU_SUCCESS, tags);
}

/** Parse `limit` and `offset` statements, format:
  * { "limit": 10, "offset": 200, ... }
  */
static std::pair<u64, u64> parse_limit_offset(boost::property_tree::ptree const& ptree) {
    u64 limit = 0ul, offset = 0ul;
    auto optlimit = ptree.get_child_optional("limit");
    if (optlimit) {
        limit = optlimit->get_value<u64>();
    }
    auto optoffset = ptree.get_child_optional("offset");
    if (optoffset) {
        limit = optoffset->get_value<u64>();
    }
    return std::make_pair(limit, offset);
}

static std::tuple<aku_Status, aku_Timestamp, aku_Timestamp> parse_range_timestamp(boost::property_tree::ptree const& ptree)
{
    aku_Timestamp begin = 0, end = 0;
    bool begin_set = false, end_set = false;
    auto range = ptree.get_child_optional("range");
    if (range) {
        for(auto child: *range) {
            if (child.first == "from") {
                auto iso_string = child.second.get_value<std::string>();
                try {
                    begin = DateTimeUtil::from_iso_string(iso_string.c_str());
                    begin_set = true;
                } catch (std::exception const& e) {
                    Logger::msg(AKU_LOG_ERROR, std::string("Can't parse begin timestmp, ") + e.what());
                }
            } else if (child.first == "to") {
                auto iso_string = child.second.get_value<std::string>();
                try {
                    end = DateTimeUtil::from_iso_string(iso_string.c_str());
                    end_set = true;
                } catch (std::exception const& e) {
                    Logger::msg(AKU_LOG_ERROR, std::string("Can't parse end timestmp, ") + e.what());
                }
            }
        }
    }
    if (!begin_set || !end_set) {
        return std::make_tuple(AKU_EQUERY_PARSING_ERROR, begin, end);
    }
    return std::make_tuple(AKU_SUCCESS, begin, end);
}

/** Parse `where` statement, format:
  * { "where": { "tag": [ "value1", "value2" ], ... }, ... }
  */
static std::tuple<aku_Status, std::vector<aku_ParamId>> parse_where_clause(boost::property_tree::ptree const& ptree,
                                                                           std::vector<std::string> metrics,
                                                                           SeriesMatcher const& matcher)
{
    aku_Status status = AKU_SUCCESS;
    std::vector<aku_ParamId> output;
    auto where = ptree.get_child_optional("where");
    if (where) {
        if (metrics.empty()) {
            Logger::msg(AKU_LOG_ERROR, "Metric is not set");
            return std::make_tuple(AKU_EQUERY_PARSING_ERROR, output);
        }
        SeriesRetreiver retreiver(metrics);
        for (auto item: *where) {
            std::string tag = item.first;
            auto idslist = item.second;
            // Read idlist
            if (!idslist.empty()) {
                std::vector<std::string> tag_values;
                for (auto idnode: idslist) {
                    tag_values.push_back(idnode.second.get_value<std::string>());
                }
                retreiver.add_tags(tag, tag_values);
            } else {
                retreiver.add_tag(tag, idslist.get_value<std::string>());
            }
        }
        std::tie(status, output) = retreiver.extract_ids(matcher);
    } else if (metrics.size()) {
        // only metric is specified
        SeriesRetreiver retreiver(metrics);
        std::tie(status, output) = retreiver.extract_ids(matcher);
    } else {
        // we need to include all series
        // were stmt is not used
        SeriesRetreiver retreiver;
        std::tie(status, output) = retreiver.extract_ids(matcher);
    }
    return std::make_tuple(status, output);
}

static std::string to_json(boost::property_tree::ptree const& ptree, bool pretty_print = true) {
    std::stringstream ss;
    boost::property_tree::write_json(ss, ptree, pretty_print);
    return ss.str();
}


// ///////////////// //
// QueryParser class //
// ///////////////// //

std::tuple<aku_Status, boost::property_tree::ptree> QueryParser::parse_json(const char* query) {
    //! C-string to streambuf adapter
    struct MemStreambuf : std::streambuf {
        MemStreambuf(const char* buf) {
            char* p = const_cast<char*>(buf);
            setg(p, p, p+strlen(p));
        }
    };

    boost::property_tree::ptree ptree;
    MemStreambuf strbuf(query);
    std::istream stream(&strbuf);
    try {
        boost::property_tree::json_parser::read_json(stream, ptree);
    } catch (boost::property_tree::json_parser_error const& e) {
        // Error, bad query
        Logger::msg(AKU_LOG_ERROR, e.what());
        return std::make_tuple(AKU_EQUERY_PARSING_ERROR, ptree);
    }
    return std::make_tuple(AKU_SUCCESS, ptree);
}

std::tuple<aku_Status, QueryKind> QueryParser::get_query_kind(boost::property_tree::ptree const& ptree) {
    aku_Status status;
    for (const auto& item: ptree) {
        if (item.first == "select") {
            std::string series;
            std::tie(status, series) = parse_select_stmt(ptree);
            if (status != AKU_SUCCESS) {
                return std::make_tuple(status, QueryKind::SELECT);
            } else if (is_meta_query(series)) {
                return std::make_tuple(AKU_SUCCESS, QueryKind::SELECT_META);
            } else {
                return std::make_tuple(AKU_SUCCESS, QueryKind::SELECT);
            }
        } else if (item.first == "aggregate") {
            return std::make_tuple(AKU_SUCCESS, QueryKind::AGGREGATE);
        } else if (item.first == "join") {
            return std::make_tuple(AKU_SUCCESS, QueryKind::JOIN);
        } else if (item.first == "group-aggregate") {
            return std::make_tuple(AKU_SUCCESS, QueryKind::GROUP_AGGREGATE);
        }
    }
    return std::make_tuple(AKU_EQUERY_PARSING_ERROR, QueryKind::SELECT);
}

aku_Status validate_query(boost::property_tree::ptree const& ptree) {
    static const std::vector<std::string> UNIQUE_STMTS = {
        "select",
        "aggregate",
        "join",
        "group-aggregate"
    };
    static const std::set<std::string> ALLOWED_STMTS = {
        "select",
        "aggregate",
        "join",
        "output",
        "order-by",
        "group-by",
        "limit",
        "offset",
        "range",
        "where",
        "group-aggregate"
    };
    std::set<std::string> keywords;
    for (const auto& item: ptree) {
        std::string keyword = item.first;
        if (ALLOWED_STMTS.count(keyword) == 0) {
            Logger::msg(AKU_LOG_ERROR, "Unexpected `" + keyword + "` statement");
            return AKU_EQUERY_PARSING_ERROR;
        }
        if (keywords.count(keyword)) {
            Logger::msg(AKU_LOG_ERROR, "Duplicate `" + keyword + "` statement");
            return AKU_EQUERY_PARSING_ERROR;
        }
        for (auto kw: UNIQUE_STMTS) {
            if (keywords.count(kw)) {
                Logger::msg(AKU_LOG_ERROR, "Statement `" + keyword + "` can't be used with `" + kw + "`");
                return AKU_EQUERY_PARSING_ERROR;
            }
        }
    }
    return AKU_SUCCESS;
}

/** Select statement should look like this:
 * { "select": "meta:*", ...}
 */
std::tuple<aku_Status, std::vector<aku_ParamId> > QueryParser::parse_select_meta_query(
        boost::property_tree::ptree const& ptree,
        SeriesMatcher const& matcher)
{
    std::vector<aku_ParamId> ids;
    aku_Status status = validate_query(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, ids);
    }
    std::string name;
    std::tie(status, name) = parse_select_stmt(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, ids);
    }
    if (!is_meta_query(name)) {
        return std::make_tuple(AKU_EQUERY_PARSING_ERROR, ids);
    }

    std::vector<std::string> metrics;
    if (name.length() > 10 && boost::starts_with(name, "meta:names")) {
        boost::erase_first(name, "meta:names:");
        metrics.push_back(name);
    }

    std::tie(status, ids) = parse_where_clause(ptree, metrics, matcher);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, ids);
    }
    return std::make_tuple(AKU_SUCCESS, ids);
}

std::tuple<aku_Status, ReshapeRequest> QueryParser::parse_select_query(
                                                    boost::property_tree::ptree const& ptree,
                                                    const SeriesMatcher &matcher)
{
    ReshapeRequest result = {};

    aku_Status status = validate_query(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    Logger::msg(AKU_LOG_INFO, "Parsing query:");
    Logger::msg(AKU_LOG_INFO, to_json(ptree, true).c_str());

    // Metric name
    std::string metric;
    std::tie(status, metric) = parse_select_stmt(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Group-by statement
    std::vector<std::string> tags;
    std::tie(status, tags) = parse_groupby(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }
    auto groupbytag = std::shared_ptr<GroupByTag>();
    if (!tags.empty()) {
        groupbytag.reset(new GroupByTag(matcher, metric, tags));
    }

    // Order-by statment
    OrderBy order;
    std::tie(status, order) = parse_orderby(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Where statement
    std::vector<aku_ParamId> ids;
    std::tie(status, ids) = parse_where_clause(ptree, {metric}, matcher);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Read timestamps
    aku_Timestamp ts_begin, ts_end;
    std::tie(status, ts_begin, ts_end) = parse_range_timestamp(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Initialize request
    result.agg.enabled = false;
    result.select.begin = ts_begin;
    result.select.end = ts_end;
    result.select.columns.push_back(Column{ids});

    result.order_by = order;

    result.group_by.enabled = static_cast<bool>(groupbytag);
    if (groupbytag) {
        result.group_by.transient_map = groupbytag->get_mapping();
        result.select.matcher = std::shared_ptr<SeriesMatcher>(groupbytag, &groupbytag->local_matcher_);
    }

    return std::make_tuple(AKU_SUCCESS, result);
}

std::tuple<aku_Status, ReshapeRequest> QueryParser::parse_aggregate_query(boost::property_tree::ptree const& ptree, SeriesMatcher const& matcher) {
    ReshapeRequest result = {};

    aku_Status status = validate_query(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    Logger::msg(AKU_LOG_INFO, "Parsing query:");
    Logger::msg(AKU_LOG_INFO, to_json(ptree, true).c_str());

    // Metric name
    std::string metric;
    std::string aggfun;
    std::tie(status, metric, aggfun) = parse_aggregate_stmt(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }
    AggregationFunction func;
    std::tie(status, func) = Aggregation::from_string(aggfun);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Group-by statement
    std::vector<std::string> tags;
    std::tie(status, tags) = parse_groupby(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }
    auto groupbytag = std::shared_ptr<GroupByTag>();
    if (!tags.empty()) {
        groupbytag.reset(new GroupByTag(matcher, metric, tags));
    }

    // Order-by statment is disallowed
    auto orderby = ptree.get_child_optional("order-by");
    if (orderby) {
        Logger::msg(AKU_LOG_INFO, "Unexpected `order-by` statement found in `aggregate` query");
        return std::make_tuple(AKU_EQUERY_PARSING_ERROR, result);
    }

    // Where statement
    std::vector<aku_ParamId> ids;
    std::tie(status, ids) = parse_where_clause(ptree, {metric}, matcher);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Read timestamps
    aku_Timestamp ts_begin, ts_end;
    std::tie(status, ts_begin, ts_end) = parse_range_timestamp(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Initialize request
    result.agg.enabled = true;
    result.agg.func = { func };

    result.select.begin = ts_begin;
    result.select.end = ts_end;
    result.select.columns.push_back(Column{ids});

    result.order_by = OrderBy::SERIES;

    result.group_by.enabled = static_cast<bool>(groupbytag);
    if (groupbytag) {
        result.group_by.transient_map = groupbytag->get_mapping();
        result.select.matcher = std::shared_ptr<SeriesMatcher>(groupbytag, &groupbytag->local_matcher_);
    }

    return std::make_tuple(AKU_SUCCESS, result);
}

static aku_Status init_matcher_in_group_aggregate(ReshapeRequest* req,
                                                  SeriesMatcher const& global_matcher,
                                                  std::string metric_name,
                                                  std::vector<AggregationFunction> const& func_names)
{
    std::vector<aku_ParamId> ids = req->select.columns.at(0).ids;
    auto matcher = std::make_shared<SeriesMatcher>();
    for (auto id: ids) {
        auto sname = global_matcher.id2str(id);
        std::string name(sname.first, sname.first + sname.second);
        if (!boost::algorithm::starts_with(name, metric_name)) {
            Logger::msg(AKU_LOG_ERROR, "Matcher initialization failed. Invalid metric name.");
            return AKU_EBAD_DATA;
        }
        auto tags = name.substr(metric_name.size());
        std::stringstream str;
        bool first = true;
        for (auto func: func_names) {
            if (first) {
                first = false;
            } else {
                str << '|';
            }
            str << metric_name << ":" << Aggregation::to_string(func);
        }
        str << tags;
        matcher->_add(str.str(), id);
    }
    req->select.matcher = matcher;
    return AKU_SUCCESS;
}

std::tuple<aku_Status, ReshapeRequest> QueryParser::parse_group_aggregate_query(boost::property_tree::ptree const& ptree, SeriesMatcher const& matcher) {
    ReshapeRequest result = {};

    aku_Status status = validate_query(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    Logger::msg(AKU_LOG_INFO, "Parsing query:");
    Logger::msg(AKU_LOG_INFO, to_json(ptree, true).c_str());

    // Metric name
    GroupAggregate gagg;
    std::tie(status, gagg) = parse_group_aggregate_stmt(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }
    if (gagg.func.empty()) {
        Logger::msg(AKU_LOG_ERROR, "Aggregation fuction is not set");
        return std::make_tuple(status, result);
    }
    if (gagg.step == 0) {
        Logger::msg(AKU_LOG_ERROR, "Step can't be zero");
        return std::make_tuple(status, result);
    }

    // Group-by statement
    std::vector<std::string> tags;
    std::tie(status, tags) = parse_groupby(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }
    auto groupbytag = std::shared_ptr<GroupByTag>();
    if (!tags.empty()) {
        groupbytag.reset(new GroupByTag(matcher, gagg.metric, tags));
    }

    // Where statement
    std::vector<aku_ParamId> ids;
    std::tie(status, ids) = parse_where_clause(ptree, {gagg.metric}, matcher);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Read timestamps
    aku_Timestamp ts_begin, ts_end;
    std::tie(status, ts_begin, ts_end) = parse_range_timestamp(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Initialize request
    result.agg.enabled = true;
    result.agg.func = gagg.func;
    result.agg.step = gagg.step;

    result.select.begin = ts_begin;
    result.select.end = ts_end;
    result.select.columns.push_back(Column{ids});

    std::tie(status, result.order_by) = parse_orderby(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    status = init_matcher_in_group_aggregate(&result, matcher, gagg.metric, gagg.func);

    result.group_by.enabled = static_cast<bool>(groupbytag);
    if (groupbytag) {
        result.group_by.transient_map = groupbytag->get_mapping();
        result.select.matcher = std::shared_ptr<SeriesMatcher>(groupbytag, &groupbytag->local_matcher_);
    }

    return std::make_tuple(AKU_SUCCESS, result);

}

static aku_Status init_matcher_in_join_query(ReshapeRequest* req,
                                             SeriesMatcher const& global_matcher,
                                             std::vector<std::string> const& metric_names)
{
    if (req->select.columns.size() < 2) {
        Logger::msg(AKU_LOG_ERROR, "Can't initialize matcher. Query is not a `JOIN` query.");
        return AKU_EBAD_ARG;
    }
    if (req->select.columns.size() != metric_names.size()) {
        Logger::msg(AKU_LOG_ERROR, "Can't initialize matcher. Invalid metric names.");
        return AKU_EBAD_ARG;
    }
    std::vector<aku_ParamId> ids = req->select.columns.at(0).ids;
    auto matcher = std::make_shared<SeriesMatcher>();
    for (auto id: ids) {
        auto sname = global_matcher.id2str(id);
        std::string name(sname.first, sname.first + sname.second);
        if (!boost::algorithm::starts_with(name, metric_names.front())) {
            Logger::msg(AKU_LOG_ERROR, "Matcher initialization failed. Invalid metric names.");
            return AKU_EBAD_DATA;
        }
        auto tags = name.substr(metric_names.front().size());
        std::stringstream str;
        bool first = true;
        for (auto metric: metric_names) {
            if (first) {
                first = false;
            } else {
                str << '|';
            }
            str << metric;
        }
        str << tags;
        matcher->_add(str.str(), id);
    }
    req->select.matcher = matcher;
    return AKU_SUCCESS;
}

std::tuple<aku_Status, ReshapeRequest> QueryParser::parse_join_query(boost::property_tree::ptree const& ptree,
                                                                     SeriesMatcher const& matcher)
{
    ReshapeRequest result = {};
    aku_Status status = validate_query(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    std::vector<std::string> metrics;
    std::tie(status, metrics) = parse_join_stmt(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Order-by statment
    OrderBy order;
    std::tie(status, order) = parse_orderby(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }
    result.order_by = order;

    // Where statement
    std::vector<aku_ParamId> ids;
    std::tie(status, ids) = parse_where_clause(ptree, metrics, matcher);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // Read timestamps
    aku_Timestamp ts_begin, ts_end;
    std::tie(status, ts_begin, ts_end) = parse_range_timestamp(ptree);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    // TODO: implement group-by
    result.group_by.enabled = false;

    // Initialize request
    result.agg.enabled = false;
    result.select.begin = ts_begin;
    result.select.end = ts_end;

    size_t ncolumns = metrics.size();
    size_t nentries = ids.size() / ncolumns;
    if (ids.size() % ncolumns != 0) {
        AKU_PANIC("Invalid `where` statement processing results");
    }
    u32 idix = 0;
    for (auto i = 0u; i < ncolumns; i++) {
        Column column;
        for (auto j = 0u; j < nentries; j++) {
            column.ids.push_back(ids.at(idix));
            idix++;
        }
        result.select.columns.push_back(column);
    }

    status = init_matcher_in_join_query(&result, matcher, metrics);
    if (status != AKU_SUCCESS) {
        return std::make_tuple(status, result);
    }

    return std::make_tuple(AKU_SUCCESS, result);
}

struct TerminalNode : QP::Node {

    InternalCursor* cursor;

    TerminalNode(InternalCursor* cur)
        : cursor(cur)
    {
    }

    // Node interface

    void complete() {
        cursor->complete();
    }

    bool put(const aku_Sample& sample) {
        if (sample.payload.type != aku_PData::MARGIN) {
            return cursor->put(sample);
        }
        return true;
    }

    void set_error(aku_Status status) {
        cursor->set_error(status);
        //throw std::runtime_error("search error detected");
    }

    int get_requirements() const {
        return TERMINAL;
    }
};


std::tuple<aku_Status, std::shared_ptr<Node>>
    make_sampler(boost::property_tree::ptree const& ptree, std::shared_ptr<Node> next)
{
    try {
        std::string name;
        name = ptree.get<std::string>("name");
        return std::make_tuple(AKU_SUCCESS, QP::create_node(name, ptree, next));
    } catch (const boost::property_tree::ptree_error& e) {
        Logger::msg(AKU_LOG_ERROR, std::string("Can't parse query: ") + e.what());
        return std::make_tuple(AKU_EQUERY_PARSING_ERROR, nullptr);
    }
}

std::tuple<aku_Status, std::vector<std::shared_ptr<Node>>> QueryParser::parse_processing_topology(
    boost::property_tree::ptree const& ptree,
    InternalCursor* cursor)
{
    // TODO: all processing steps are bypassed now, this should be fixed
    auto terminal = std::make_shared<TerminalNode>(cursor);
    std::vector<std::shared_ptr<Node>> result;

    auto limoff = parse_limit_offset(ptree);
    if (limoff.first != 0 || limoff.second != 0) {
        auto node = std::make_shared<QP::Limiter>(limoff.first, limoff.second, terminal);
        result.push_back(node);
    }

    result.push_back(terminal);
    return std::make_tuple(AKU_SUCCESS, result);
}

}}  // namespace
