/**
 * Copyright (c) 2015 Eugene Lazin <4lazin@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "queryprocessor.h"
#include "util.h"
#include "datetime.h"
#include "anomalydetector.h"
#include "saxencoder.h"

#include <random>
#include <algorithm>
#include <unordered_set>
#include <set>

#include <boost/lexical_cast.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/exception/diagnostic_information.hpp>

// Include query processors
#include "query_processing/anomaly.h"
#include "query_processing/filterbyid.h"
#include "query_processing/paa.h"
#include "query_processing/randomsamplingnode.h"
#include "query_processing/sax.h"
#include "query_processing/spacesaver.h"

namespace Akumuli {
namespace QP {

//                                   //
//         Factory methods           //
//                                   //


static std::shared_ptr<Node> make_sampler(boost::property_tree::ptree const& ptree,
                                          std::shared_ptr<Node> next,
                                          aku_logger_cb_t logger)
{
    try {
        std::string name;
        name = ptree.get<std::string>("name");
        return QP::create_node(name, ptree, next);
    } catch (const boost::property_tree::ptree_error&) {
        QueryParserError except("invalid sampler description");
        BOOST_THROW_EXCEPTION(except);
    }
}



struct RegexFilter : IQueryFilter {
    std::string regex_;
    std::unordered_set<aku_ParamId> ids_;
    StringPool const& spool_;
    StringPoolOffset offset_;
    size_t prev_size_;

    RegexFilter(std::string regex, StringPool const& spool)
        : regex_(regex)
        , spool_(spool)
        , offset_{}
        , prev_size_(spool.size())
    {
        refresh();
    }

    void refresh() {
        std::vector<StringPool::StringT> results = spool_.regex_match(regex_.c_str(), &offset_);
        for (StringPool::StringT item: results) {
            auto id = StringTools::extract_id_from_pool(item);
            ids_.insert(id);
        }
    }

    virtual std::vector<aku_ParamId> get_ids() {
        std::vector<aku_ParamId> result;
        std::copy(ids_.begin(), ids_.end(), std::back_inserter(result));
        return result;
    }

    virtual FilterResult apply(aku_ParamId id) {
        // Atomic operation, can be a source of contention
        if (spool_.size() != prev_size_) {
            refresh();
        }
        return ids_.count(id) != 0 ? PROCESS : SKIP_THIS;
    }
};


GroupByStatement::GroupByStatement()
    : step_(0)
    , first_hit_(true)
    , lowerbound_(AKU_MIN_TIMESTAMP)
    , upperbound_(AKU_MIN_TIMESTAMP)
{
}

GroupByStatement::GroupByStatement(aku_Timestamp step)
    : step_(step)
    , first_hit_(true)
    , lowerbound_(AKU_MIN_TIMESTAMP)
    , upperbound_(AKU_MIN_TIMESTAMP)
{
}

GroupByStatement::GroupByStatement(const GroupByStatement& other)
    : step_(other.step_)
    , first_hit_(other.first_hit_)
    , lowerbound_(other.lowerbound_)
    , upperbound_(other.upperbound_)
{
}

GroupByStatement& GroupByStatement::operator = (const GroupByStatement& other) {
    step_ = other.step_;
    first_hit_ = other.first_hit_;
    lowerbound_ = other.lowerbound_;
    upperbound_ = other.upperbound_;
    return *this;
}

bool GroupByStatement::put(aku_Sample const& sample, Node& next) {
    if (step_) {
        aku_Timestamp ts = sample.timestamp;
        if (AKU_UNLIKELY(first_hit_ == true)) {
            first_hit_ = false;
            aku_Timestamp aligned = ts / step_ * step_;
            lowerbound_ = aligned;
            upperbound_ = aligned + step_;
        }
        if (ts >= upperbound_) {
            // Forward direction
            aku_Sample empty = EMPTY_SAMPLE;
            empty.timestamp = upperbound_;
            if (!next.put(empty)) {
                return false;
            }
            lowerbound_ += step_;
            upperbound_ += step_;
        } else if (ts < lowerbound_) {
            // Backward direction
            aku_Sample empty = EMPTY_SAMPLE;
            empty.timestamp = upperbound_;
            if (!next.put(empty)) {
                return false;
            }
            lowerbound_ -= step_;
            upperbound_ -= step_;
        }
    }
    return next.put(sample);
}

bool GroupByStatement::empty() const {
    return step_ == 0;
}

ScanQueryProcessor::ScanQueryProcessor(std::vector<std::shared_ptr<Node>> nodes,
                                       std::string metric,
                                       aku_Timestamp begin,
                                       aku_Timestamp end, std::shared_ptr<IQueryFilter> filter,
                                       GroupByStatement groupby)
    : lowerbound_(std::min(begin, end))
    , upperbound_(std::max(begin, end))
    , direction_(begin > end ? AKU_CURSOR_DIR_BACKWARD : AKU_CURSOR_DIR_FORWARD)
    , metric_(metric)
    , namesofinterest_(StringTools::create_table(0x1000))
    , groupby_(groupby)
    , filter_(filter)
{
    if (nodes.empty()) {
        AKU_PANIC("`nodes` shouldn't be empty")
    }
    root_node_ = nodes.at(0);

    // validate query processor data
    if (groupby_.empty()) {
        for (auto ptr: nodes) {
            if ((ptr->get_requirements() & Node::GROUP_BY_REQUIRED) != 0) {
                NodeException err("`group_by` required");  // TODO: more detailed error message
                BOOST_THROW_EXCEPTION(err);
            }
        }
    }

    int nnormal = 0;
    for (auto it = nodes.rbegin(); it != nodes.rend(); it++) {
        if (((*it)->get_requirements() & Node::TERMINAL) != 0) {
            if (nnormal != 0) {
                NodeException err("invalid sampling order");  // TODO: more detailed error message
                BOOST_THROW_EXCEPTION(err);
            }
        } else {
            nnormal++;
        }
    }
}

IQueryFilter& ScanQueryProcessor::filter() {
    return *filter_;
}

bool ScanQueryProcessor::start() {
    return true;
}

bool ScanQueryProcessor::put(const aku_Sample &sample) {
    return groupby_.put(sample, *root_node_);
}

void ScanQueryProcessor::stop() {
    root_node_->complete();
}

void ScanQueryProcessor::set_error(aku_Status error) {
    std::cerr << "ScanQueryProcessor->error" << std::endl;
    root_node_->set_error(error);
}

aku_Timestamp ScanQueryProcessor::lowerbound() const {
    return lowerbound_;
}

aku_Timestamp ScanQueryProcessor::upperbound() const {
    return upperbound_;
}

int ScanQueryProcessor::direction() const {
    return direction_;
}

MetadataQueryProcessor::MetadataQueryProcessor(std::shared_ptr<IQueryFilter> flt, std::shared_ptr<Node> node)
    : filter_(flt)
    , root_(node)
{
}

aku_Timestamp MetadataQueryProcessor::lowerbound() const {
    return AKU_MAX_TIMESTAMP;
}

aku_Timestamp MetadataQueryProcessor::upperbound() const {
    return AKU_MAX_TIMESTAMP;
}

int MetadataQueryProcessor::direction() const {
    return AKU_CURSOR_DIR_FORWARD;
}

IQueryFilter& MetadataQueryProcessor::filter() {
    return *filter_;
}

bool MetadataQueryProcessor::start() {
    for (auto id: filter_->get_ids()) {
        aku_Sample s;
        s.paramid = id;
        s.timestamp = 0;
        s.payload.type = aku_PData::PARAMID_BIT;
        s.payload.size = sizeof(aku_Sample);
        if (!root_->put(s)) {
            return false;
        }
    }
    return true;
}

bool MetadataQueryProcessor::put(const aku_Sample &sample) {
    // no-op
    return false;
}

void MetadataQueryProcessor::stop() {
    root_->complete();
}

void MetadataQueryProcessor::set_error(aku_Status error) {
    root_->set_error(error);
}



//                          //
//                          //
//   Build query processor  //
//                          //
//                          //

static boost::optional<std::string> parse_select_stmt(boost::property_tree::ptree const& ptree, aku_logger_cb_t logger) {
    auto select = ptree.get_child_optional("select");
    if (select && select->empty()) {
        // simple select query
        auto str = select->get_value<std::string>("");
        if (str == "names") {
            // the only supported select query for now
            return str;
        }
        (*logger)(AKU_LOG_ERROR, "Invalid `select` query");
        auto rte = std::runtime_error("Invalid `select` query");
        BOOST_THROW_EXCEPTION(rte);
    }
    return boost::optional<std::string>();
}

static QP::GroupByStatement parse_groupby(boost::property_tree::ptree const& ptree,
                                          aku_logger_cb_t logger) {
    aku_Timestamp duration = 0u;
    auto groupby = ptree.get_child_optional("group-by");
    if (groupby) {
        for(auto child: *groupby) {
            if (child.first == "time") {
                std::string str = child.second.get_value<std::string>();
                duration = DateTimeUtil::parse_duration(str.c_str(), str.size());
            }
        }
    }
    return QP::GroupByStatement(duration);
}

static std::string parse_metric(boost::property_tree::ptree const& ptree,
                                aku_logger_cb_t logger) {
    std::string metric;
    auto opt = ptree.get_child_optional("metric");
    if (opt) {
        auto single = opt->get_value<std::string>();
        metric = single;
    }
    return metric;
}

static aku_Timestamp parse_range_timestamp(boost::property_tree::ptree const& ptree,
                                           std::string const& name,
                                           aku_logger_cb_t logger) {
    auto range = ptree.get_child("range");
    for(auto child: range) {
        if (child.first == name) {
            auto iso_string = child.second.get_value<std::string>();
            auto ts = DateTimeUtil::from_iso_string(iso_string.c_str());
            return ts;
        }
    }
    std::stringstream fmt;
    fmt << "can't find `" << name << "` tag inside the query";
    QueryParserError error(fmt.str().c_str());
    BOOST_THROW_EXCEPTION(error);
}

static std::shared_ptr<RegexFilter> parse_where_clause(boost::property_tree::ptree const& ptree,
                                                       std::string metric,
                                                       std::string pred,
                                                       StringPool const& pool,
                                                       aku_logger_cb_t logger)
{
    if (metric.empty()) {
        // metric wasn't set so we should match all metrics
        metric = "\\w+";
    }
    std::shared_ptr<RegexFilter> result;
    bool not_set = false;
    auto where = ptree.get_child_optional("where");
    if (where) {
        for (auto item: *where) {
            bool firstitem = true;
            std::stringstream series_regexp;
            std::string tag = item.first;
            auto idslist = item.second;
            // Read idlist
            for (auto idnode: idslist) {
                std::string value = idnode.second.get_value<std::string>();
                if (firstitem) {
                    firstitem = false;
                    series_regexp << "(?:";
                } else {
                    series_regexp << "|";
                }
                series_regexp << "(" << metric << R"((?:\s\w+=\w+)*\s)"
                              << tag << "=" << value << R"((?:\s\w+=\w+)*))";
            }
            series_regexp << ")";
            std::string regex = series_regexp.str();
            result = std::make_shared<RegexFilter>(regex, pool);
        }
    } else {
        not_set = true;
    }
    if (not_set) {
        // we need to include all series from this metric
        std::stringstream series_regexp;
        series_regexp << "" << metric << R"((\s\w+=\w+)*)";
        std::string regex = series_regexp.str();
        result = std::make_shared<RegexFilter>(regex, pool);
    }
    return result;
}

static std::string to_json(boost::property_tree::ptree const& ptree, bool pretty_print = true) {
    std::stringstream ss;
    boost::property_tree::write_json(ss, ptree, pretty_print);
    return ss.str();
}

std::shared_ptr<QP::IQueryProcessor> Builder::build_query_processor(const char* query,
                                                                    std::shared_ptr<QP::Node> terminal,
                                                                    const SeriesMatcher &matcher,
                                                                    aku_logger_cb_t logger) {
    namespace pt = boost::property_tree;
    using namespace QP;


    const auto NOSAMPLE = std::make_pair<std::string, size_t>("", 0u);

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
        pt::json_parser::read_json(stream, ptree);
    } catch (pt::json_parser_error& e) {
        // Error, bad query
        (*logger)(AKU_LOG_ERROR, e.what());
        throw QueryParserError(e.what());
    }

    logger(AKU_LOG_INFO, "Parsing query:");
    logger(AKU_LOG_INFO, to_json(ptree, true).c_str());

    try {
        // Read groupby statement
        auto groupby = parse_groupby(ptree, logger);

        // Read metric name
        auto metric = parse_metric(ptree, logger);

        // Read select statment
        auto select = parse_select_stmt(ptree, logger);

        // Read sampling method
        auto sampling_params = ptree.get_child_optional("sample");

        // Read where clause
        auto filter = parse_where_clause(ptree, metric, "in", matcher.pool, logger);

        if (sampling_params && select) {
            (*logger)(AKU_LOG_ERROR, "Can't combine select and sample statements together");
            auto rte = std::runtime_error("`sample` and `select` can't be used together");
            BOOST_THROW_EXCEPTION(rte);
        }

        // Build topology
        std::shared_ptr<Node> next = terminal;
        std::vector<std::shared_ptr<Node>> allnodes = { next };
        if (!select) {
            // Read timestamps
            auto ts_begin = parse_range_timestamp(ptree, "from", logger);
            auto ts_end = parse_range_timestamp(ptree, "to", logger);

            if (sampling_params) {
                for (auto i = sampling_params->rbegin(); i != sampling_params->rend(); i++) {
                        next = make_sampler(i->second, next, logger);
                        allnodes.push_back(next);
                }
            }
            std::reverse(allnodes.begin(), allnodes.end());
            // Build query processor
            return std::make_shared<ScanQueryProcessor>(allnodes, metric, ts_begin, ts_end, filter, groupby);
        }
        return std::make_shared<MetadataQueryProcessor>(filter, next);

    } catch(std::exception const& e) {
        (*logger)(AKU_LOG_ERROR, e.what());
        throw QueryParserError(e.what());
    }
}

}} // namespace
