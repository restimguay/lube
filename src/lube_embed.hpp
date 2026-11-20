/*
 * lube_embed.hpp - Lube inside a C++ program: classes over lube_embed.h.
 *
 *   lube::VM vm;
 *   vm.def("add", [](lube::VM &, const std::vector<lube::Value> &a) {
 *       return lube::Value(a[0].number() + a[1].number());
 *   }, 2, 2);
 *   vm.run("fn twice(x) => add(x, x)");
 *   double ten = vm.call("twice", {5}).number();
 *
 * A Value owns its Lube value (copies share it, as Lube's do), so make the
 * VM first and its values after: they must go before it does. Errors
 * become lube::Error; an exception thrown by a function given with def()
 * becomes a Lube error, which the Lube code can catch.
 */
#ifndef LUBE_EMBED_HPP
#define LUBE_EMBED_HPP

#include "lube_embed.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lube {

class Value {
  public:
    Value() : v_(lube_api()->nothing()) {}
    Value(double x) : v_(lube_api()->number_value(x)) {}
    Value(int x) : v_(lube_api()->number_value(x)) {}
    Value(bool b) : v_(lube_api()->boolean(b)) {}
    Value(const char *) = delete;  // (text needs its VM: vm.str("..."))
    Value(const Value &o) : v_(lube_api()->retain(o.v_)) {}
    Value(Value &&o) noexcept : v_(o.v_) { o.v_ = lube_api()->nothing(); }
    Value &operator=(Value o) noexcept {
        std::swap(v_, o.v_);
        return *this;
    }
    ~Value() { lube_api()->release(v_); }

    static Value take(LubeValue v) {  // takes over a reference (what lube_get and friends give)
        Value x;
        x.v_ = v;
        return x;
    }
    static Value borrow(LubeValue v) { return take(lube_api()->retain(v)); }

    int kind() const { return lube_api()->kind(v_); }
    bool is_nothing() const { return kind() == LUBE_NOTHING; }
    double number() const { return lube_api()->number(v_); }
    bool truthy() const { return lube_api()->truthy(v_) != 0; }
    std::string str() const {  // text ("" if it is not text)
        size_t n;
        const char *s = lube_api()->str(v_, &n);
        return s ? std::string(s, n) : std::string();
    }
    std::string type_name() const { return lube_api()->type_name(v_); }
    size_t size() const { return lube_api()->list_len(v_); }        // a list's length
    Value operator[](size_t i) const { return borrow(lube_api()->list_get(v_, i)); }

    const LubeValue &raw() const { return v_; }
    LubeValue release() {  // hands over the reference
        LubeValue v = v_;
        v_ = lube_api()->nothing();
        return v;
    }

  private:
    LubeValue v_;
};

class Error : public std::runtime_error {  // what lube_error_message says
  public:
    explicit Error(const std::string &message) : std::runtime_error(message) {}
};

class VM;
typedef std::function<Value(VM &vm, const std::vector<Value> &args)> Function;

class VM {
  public:
    VM() : vm_(lube_open()) {}
    ~VM() { lube_close(vm_); }
    VM(const VM &) = delete;
    VM &operator=(const VM &) = delete;

    // Runs code: true, or false when it called exit() (exit_code() says with what). Errors throw.
    bool run(const std::string &code, const std::string &name = "<host>") {
        return check(lube_run(vm_, code.c_str(), name.c_str()));
    }
    bool run_script(const std::string &path) { return check(lube_run_script(vm_, path.c_str())); }
    int exit_code() const { return lube_exit_code(vm_); }

    Value get(const std::string &name) const {  // nothing if there is no such global
        LubeValue v;
        return lube_get(vm_, name.c_str(), &v) ? Value::take(v) : Value();
    }
    void set(const std::string &name, Value v) { check(lube_set(vm_, name.c_str(), v.release())); }
    Value call(const std::string &name, const std::vector<Value> &args = std::vector<Value>()) {
        std::vector<LubeValue> raw;
        for (size_t i = 0; i < args.size(); i++) raw.push_back(args[i].raw());
        LubeValue r;
        int status = lube_call_global(vm_, name.c_str(), (int)raw.size(), raw.empty() ? NULL : &raw[0], &r);
        return check(status) ? Value::take(r) : Value();
    }

    Value str(const std::string &s) { return Value::take(lube_api()->new_str(vm_, s.data(), s.size())); }
    Value list(const std::vector<Value> &items) {
        LubeValue l = lube_api()->new_list(vm_);
        for (size_t i = 0; i < items.size(); i++) lube_api()->list_push(l, lube_api()->retain(items[i].raw()));
        return Value::take(l);
    }

    // A global function: fn(vm, args). max_args -1 takes any number.
    void def(const std::string &name, Function fn, int min_args = 0, int max_args = -1) {
        fns_.push_back(std::unique_ptr<Entry>(new Entry(this, fn)));
        lube_function(vm_, name.c_str(), &VM::call_entry, min_args, max_args, fns_.back().get());
    }

    LubeVM *raw() const { return vm_; }

  private:
    struct Entry {
        Entry(VM *v, const Function &f) : vm(v), fn(f) {}
        VM *vm;
        Function fn;
    };

    static LubeValue call_entry(LubeVM *vm, int argc, const LubeValue *argv) {
        Entry *e = static_cast<Entry *>(lube_api()->function_data(vm));
        try {
            std::vector<Value> args;
            for (int i = 0; i < argc; i++) args.push_back(Value::borrow(argv[i]));
            return e->fn(*e->vm, args).release();
        } catch (const std::exception &x) {  // (never through Lube's C)
            return lube_api()->error(vm, "%s", x.what());
        } catch (...) {
            return lube_api()->error(vm, "a C++ exception");
        }
    }

    bool check(int status) const {
        if (status == LUBE_ERROR) throw Error(lube_error_message(vm_));
        return status == LUBE_OK;
    }

    LubeVM *vm_;
    std::vector<std::unique_ptr<Entry> > fns_;
};

}  // namespace lube

#endif
