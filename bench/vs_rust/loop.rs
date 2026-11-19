// A tight loop over locals.
fn run(n: u64) -> u64 {
    let mut total = 0;
    let mut i = 0;
    while i < n {
        total += i % 7;
        i += 1;
    }
    total
}
fn main() {
    println!("{}", run(30_000_000));
}
