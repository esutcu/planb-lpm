// binding.cpp — pybind11 bindings for lpm6::Tree and lpm6::Dynamic.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include "../include/lpm6.hpp"
#include "../src/ipv6_util.hpp"

namespace py = pybind11;

namespace {

std::vector<lpm6::Entry>
to_entries(const std::vector<std::tuple<std::string, int, int>>& in) {
    std::vector<lpm6::Entry> fib;
    fib.reserve(in.size());
    for (const auto& [s, len, nh] : in) {
        if (len < 0 || len > 64) continue;
        std::uint64_t p = lpm6::parse_ipv6(s.c_str());
        if (p == ~std::uint64_t(0)) continue;
        fib.push_back({p, len, nh});
    }
    return fib;
}

std::uint64_t parse_or_throw(const std::string& s) {
    std::uint64_t v = lpm6::parse_ipv6(s.c_str());
    if (v == ~std::uint64_t(0))
        throw py::value_error("invalid IPv6 address: " + s);
    return v;
}

} // namespace

PYBIND11_MODULE(planb_lpm, m) {
    m.doc() = "PlanB IPv6 longest-prefix-match (AVX-512 accelerated when available)";

    py::class_<lpm6::Tree>(m, "Tree")
        .def(py::init<>())
        .def("build",
            [](lpm6::Tree& t,
               const std::vector<std::tuple<std::string,int,int>>& fib) {
                t.build(to_entries(fib));
            },
            py::arg("fib"),
            "Build from a list of (prefix, prefix_len, next_hop) tuples.")
        .def("lookup",
            [](const lpm6::Tree& t, const std::string& addr) {
                return t.lookup(parse_or_throw(addr));
            },
            py::arg("addr"))
        .def("lookup_many",
            [](const lpm6::Tree& t, const std::vector<std::string>& addrs) {
                std::vector<int> out(addrs.size(), 0);
                for (std::size_t i = 0; i < addrs.size(); ++i) {
                    std::uint64_t v = lpm6::parse_ipv6(addrs[i].c_str());
                    out[i] = (v == ~std::uint64_t(0)) ? -1 : t.lookup(v);
                }
                return out;
            },
            py::arg("addrs"))
        .def_property_readonly("depth",       &lpm6::Tree::depth)
        .def_property_readonly("total_keys",  &lpm6::Tree::total_keys)
        .def_property_readonly("edge_count",  &lpm6::Tree::edge_count);

    py::class_<lpm6::Dynamic>(m, "Dynamic")
        .def(py::init<>())
        .def("load",
            [](lpm6::Dynamic& d,
               const std::vector<std::tuple<std::string,int,int>>& fib) {
                d.load(to_entries(fib));
            },
            py::arg("fib"))
        .def("insert",
            [](lpm6::Dynamic& d, const std::string& prefix, int len, int nh) {
                d.insert(parse_or_throw(prefix), len, nh);
            },
            py::arg("prefix"), py::arg("len"), py::arg("next_hop"))
        .def("remove",
            [](lpm6::Dynamic& d, const std::string& prefix, int len) {
                return d.remove(parse_or_throw(prefix), len);
            },
            py::arg("prefix"), py::arg("len"))
        .def("update",
            [](lpm6::Dynamic& d, const std::string& prefix, int len, int nh) {
                return d.update(parse_or_throw(prefix), len, nh);
            },
            py::arg("prefix"), py::arg("len"), py::arg("next_hop"))
        .def("lookup",
            [](const lpm6::Dynamic& d, const std::string& addr) {
                return d.lookup(parse_or_throw(addr));
            },
            py::arg("addr"))
        .def("__len__", &lpm6::Dynamic::size);
}
