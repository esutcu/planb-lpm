"""Minimal demo of the planb_lpm Python API."""

from planb_lpm import Dynamic, Tree

fib = [
    ("::",               0,  1),   # default route
    ("2001:db8::",       32, 10),
    ("2001:db8:1::",     48, 11),
    ("2001:db8:1:abcd::",64, 12),
    ("fe80::",           10, 20),
]

tree = Tree()
tree.build(fib)

print("depth          :", tree.depth)
print("edge_count     :", tree.edge_count)

print("lookup 2001:db8::1        ->", tree.lookup("2001:db8::1"))
print("lookup 2001:db8:1::beef   ->", tree.lookup("2001:db8:1::beef"))
print("lookup 2001:db8:1:abcd::1 ->", tree.lookup("2001:db8:1:abcd::1"))
print("lookup fe80::1            ->", tree.lookup("fe80::1"))
print("lookup 3000::1            ->", tree.lookup("3000::1"))

dyn = Dynamic()
dyn.load(fib)
dyn.insert("2001:db8:2::", 48, 99)
dyn.remove("fe80::", 10)
print("dynamic: 2001:db8:2::1    ->", dyn.lookup("2001:db8:2::1"))
print("dynamic: fe80::1          ->", dyn.lookup("fe80::1"))
print("dynamic size              :", len(dyn))
