// Sorting a million numbers.
fn main() {
    let mut xs: Vec<u64> = Vec::new();
    let mut x: u64 = 42;
    for _ in 0..1_000_000 {
        x = x * 16807 % 2147483647;
        xs.push(x);
    }
    xs.sort();
    println!("{} {} {}", xs[0], xs[500_000], xs[999_999]);
}
