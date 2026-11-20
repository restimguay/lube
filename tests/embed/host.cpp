// host.cpp - Lube inside a C++ program, through lube_embed.hpp (tests/cli.sh builds and runs it)
#include "lube_embed.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    lube::VM vm;  // (first: the values below go before it does)
    std::map<std::string, double> stats;

    std::cout << "-- running code" << std::endl;
    vm.run("var level = 1\nfn greet(who) => \"hello, {who}\"\nprint(\"ready\")", "setup");
    std::cout << "level " << vm.get("level").number() << ", " << vm.call("greet", {vm.str("C++")}).str() << std::endl;

    std::cout << "-- functions in C++" << std::endl;
    vm.def("record", [&stats](lube::VM &, const std::vector<lube::Value> &args) {  // (a lambda, with what it holds)
        stats[args[0].str()] = args[1].number();
        return lube::Value(static_cast<int>(stats.size()));
    }, 2, 2);
    vm.def("split_words", [](lube::VM &v, const std::vector<lube::Value> &args) {
        std::vector<lube::Value> words;
        std::string text = args[0].str(), word;
        for (char c : text + " ") {
            if (c == ' ' && !word.empty()) {
                words.push_back(v.str(word));
                word.clear();
            } else if (c != ' ') {
                word += c;
            }
        }
        return v.list(words);
    }, 1, 1);
    vm.def("must_be_positive", [](lube::VM &, const std::vector<lube::Value> &args) -> lube::Value {
        if (args[0].number() <= 0) throw std::invalid_argument("must_be_positive: " + args[0].type_name() + " too small");
        return args[0];
    });
    vm.def("weird", [](lube::VM &, const std::vector<lube::Value> &) -> lube::Value { throw 42; });
    vm.run("print(record(\"hp\", 10), record(\"mp\", 4), split_words(\"  two   words \"))\n"
           "try { must_be_positive(-1) } catch e { print(\"caught:\", e) }\n"
           "try { weird() } catch e { print(\"caught:\", e) }\n"
           "print(must_be_positive(3))",
           "functions");
    std::cout << "stats: hp " << stats["hp"] << ", mp " << stats["mp"] << std::endl;

    std::cout << "-- values" << std::endl;
    vm.set("pos", vm.list({lube::Value(1.5), lube::Value(-2), lube::Value(true)}));
    lube::Value pos = vm.get("pos"), copy = pos;
    std::cout << pos.type_name() << " of " << pos.size() << ": " << pos[0].number() << " " << pos[1].number() << " "
              << pos[2].truthy() << " " << copy[2].type_name() << std::endl;
    lube::Value moved = std::move(copy);
    std::cout << moved.size() << " " << copy.is_nothing() << " " << vm.get("nope").is_nothing() << std::endl;
    lube::Value a(1.0), b = a;
    b = vm.str("text");
    std::cout << a.number() << " " << b.str() << " " << a.kind() << " " << b.kind() << std::endl;

    std::cout << "-- errors" << std::endl;
    try {
        vm.run("let x = 1 / [1]", "broken");
    } catch (const lube::Error &e) {
        std::cout << "lube::Error: " << e.what();
    }
    try {
        vm.call("greet");
    } catch (const lube::Error &e) {
        std::cout << "lube::Error: " << e.what();
    }

    std::cout << "-- exit" << std::endl;
    bool ran = vm.run("exit(5)", "exits");
    std::cout << "ran to its end: " << ran << ", exit code " << vm.exit_code() << std::endl;
    vm.run("fn quit() { exit(6) }");
    std::cout << "call: " << vm.call("quit").is_nothing() << ", exit code " << vm.exit_code() << std::endl;
    std::cout << "script: " << vm.run_script("tests/embed/script_cpp.lube") << std::endl;
    std::cout << "-- done" << std::endl;
    return 0;
}
