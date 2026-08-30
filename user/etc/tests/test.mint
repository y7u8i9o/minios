# mint interpreter test
fn fib(n) {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
fn greet(name) {
    return "hello, " + name
}
print("fib", fib(15))
print(greet("mint"))
total = 0
for i in range(1, 11) {
    if i % 2 == 0 { continue }
    total = total + i
}
print("odd sum", total)
i = 0
while 1 {
    i = i + 1
    if i == 5 { break }
}
print("loop stopped at", i)
s = "abcdef"
print(len(s), substr(s, 2, 3), ord("A"), chr(66), str(42) + "!", int("17") * 2)
print("cmp", "abc" < "abd", 3 >= 3, !0, 1 && 0, 0 || 2)
print("args", argc(), argv(1))
writefile("/mint.out", "written by mint\n")
print(readfile("/mint.out") == "written by mint\n")
counter = 0
fn bump() {
    counter = counter + 1
    local = 5
    return local
}
bump()
bump()
print("counter", counter)
x = 7
if x > 10 { print("big") } elif x > 5 { print("medium") } else { print("small") }
print(-3 * 2, 17 / 5, 17 % 5)
