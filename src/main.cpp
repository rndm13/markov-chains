#include <algorithm>
#include <boost/json.hpp>
#include <cassert>
#include <chrono>
#include <fmt/format.h>
#include <fmt/os.h>
#include <fmt/ostream.h>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <random>
#include <ranges>
#include <regex>
#include <string>
#include <unordered_map>

namespace mkv {
template <typename ValueType> struct Markov {
  struct Node {
    // data held in the Node
    const ValueType value{};

    // unique id
    uint32_t id{};

    // key: next node (nullptr == end)
    // val: count of times leading
    std::unordered_map<std::shared_ptr<Node>, uint32_t> edges{};
    Node(ValueType _v) : value(_v) {
      static uint32_t max_node_id{};
      id = ++max_node_id;
    }
    Node(const Node &_n) : value(_n->value), id(_n->edges), edges(_n->edges) {}
  };

  // all nodes
  std::unordered_map<ValueType, std::shared_ptr<Node>> nodes{};

  // count of starting nodes
  std::unordered_map<std::shared_ptr<Node>, uint32_t> start_edges{};

  void connect_nodes(const std::shared_ptr<Node> &a,
                     const std::shared_ptr<Node> &b) {
    // either a or b must be an element
    assert(a || b);

    // b is after a
    if (a)
      ++(a->edges[b]);

    // a == nullptr => b is starting element
    else
      ++start_edges[b];
  }

  void add_chain(std::ranges::range auto input) {
    std::shared_ptr<Node> fst_ptr{nullptr};
    auto connect = [&](const std::shared_ptr<Node> &snd_ptr) mutable {
      connect_nodes(fst_ptr, snd_ptr);
      fst_ptr = snd_ptr;
    };
    auto to_shared_ptr = [&](const ValueType &vt) {
      if (!nodes.contains(vt))
        nodes.emplace(vt, std::make_shared<Node>(vt));
      return nodes.at(vt);
    };

    std::ranges::for_each(input | std::ranges::views::transform(to_shared_ptr),
                          connect);

    connect(std::shared_ptr<Node>(nullptr));
  }

  Markov() {}
  Markov(std::ranges::range auto input) { add_chain(input); }

  std::shared_ptr<Node> get_next_node(
      const std::unordered_map<std::shared_ptr<Node>, uint32_t> &prob) {

    std::vector<std::shared_ptr<Node>> prob_nodes{};
    for (auto v : prob)
      prob_nodes.push_back(v.first);

    std::random_device r;
    std::default_random_engine generator(r());

    auto it = prob.begin();

    std::discrete_distribution<> dist(prob.size(), 0, 1,
                                      [&it](float) { return (it++)->second; });

    return prob_nodes.at(dist(generator));
  }

  std::ranges::range auto generate() {
    std::vector<ValueType> result{};
    auto cur = get_next_node(start_edges);
    while (cur) {
      result.push_back(cur->value);
      cur = get_next_node(cur->edges);
    }
    return result;
  }

  auto make_graphviz() const {
    auto fmt_node = [](const std::shared_ptr<Node> &fst) {
      auto con_graphviz =
          [fst](const std::pair<std::shared_ptr<Node>, uint32_t> &snd) {
            if (!snd.first)
              return fmt::format("{} -- end [width = \"{}.0\"];", fst->id,
                                 snd.second);
            return fmt::format("{} -- {} [width = \"{}.0\"];", fst->id,
                               snd.first->id, snd.second); // get values of it
          };
      return fmt::join(std::views::transform(fst->edges, // get shared_ptr<Node>
                                             con_graphviz),
                       "\n");
    };
    return fmt::format(
        "graph G {{\nstart [shape = Msquare]\nend [shape = "
        "Msquare]\n{0}\n{1}\n{2}\n}}\n",
        fmt::join(std::views::transform(std::views::values(nodes),
                                        [](const auto &n) {
                                          return fmt::format(
                                              "{0} [label = \"{1}\"];", n->id,
                                              n->value);
                                        }),
                  "\n"),                 // 0 setting up node names
        fmt::join(std::views::transform( // 1 connections from start
                      start_edges,
                      [](const auto &out) {
                        return fmt::format("start -- {} [label = \"{}.0\"];",
                                           out.first->id, out.second);
                      }),
                  "\n"),
        fmt::join(std::views::transform(std::views::values(nodes),
                                        fmt_node), // 3 internal connections
                  "\n"));
  }
};
} // namespace mkv

std::regex white_space_rgx(R"abc(\s)abc");

std::string slurp(std::ifstream &in) {
  std::ostringstream sstr;
  sstr << in.rdbuf();
  return sstr.str();
}

std::vector<std::string> to_words(const std::string &str) {
  return {
      std::sregex_token_iterator(str.begin(), str.end(), white_space_rgx, -1),
      std::sregex_token_iterator()};
}

std::string file_type(const std::string &file_name) {
  auto dot = file_name.rfind(".");
  if (dot == std::string::npos)
    return "";
  return file_name.substr(dot + 1, INT_MAX);
}

void parse_file(mkv::Markov<std::string> &markov_chains,
                const std::string &file_name) {
  std::ifstream fin(file_name);
  std::string ft = file_type(file_name);

  auto add_to_chains = [&](const std::string &text) {
    if (text == "")
      return;
    auto words = to_words(text);
    if (words.size() >= 5)
      markov_chains.add_chain(words);
  };

  fmt::print("Parsing file \"{}\".\n", file_name);

  if (ft == "txt") {
    std::string str;
    while (std::getline(fin, str))
      add_to_chains(str);
    return;
  }

  if (ft == "json") {
    namespace json = boost::json;
    json::error_code ec;
    auto parsed_json = json::parse(slurp(fin), ec);

    if (ec)
      fmt::print("Parsing failed: {}\n", ec.message());

    auto message_array = parsed_json.as_object()["messages"].as_array();

    for (auto &message : message_array) {
        auto text_value = message.as_object()["text"];
        if (text_value.kind() == json::kind::string)
          add_to_chains(json::value_to<std::string>(text_value));
    }
    return;
  }
  fmt::print("Unknown \"{}\" file type, skipping", ft);
}

int main(int argc, char **argv) {
  if (argc <= 1) {
    fmt::print("USAGE: markov file_names\n");
    return 1;
  }

  mkv::Markov<std::string> markov_chains;

  for (int ind = 1; ind < argc; ++ind) {

    parse_file(markov_chains, argv[ind]);
  }

  auto out = fmt::output_file("markov.dot");
  out.print("{}", markov_chains.make_graphviz());

  for (;;) {
    fmt::print("{}\n-------------------\n", fmt::join(markov_chains.generate(), " "));
  }
  return 0;
}
