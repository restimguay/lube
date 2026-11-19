// Method calls that change an object.
struct Counter {
    n: u64,
}
impl Counter {
    fn add(&mut self, k: u64) {
        self.n += k;
    }
    fn get(&self) -> u64 {
        self.n
    }
}
fn main() {
    let mut c = Counter { n: 0 };
    for i in 0..5_000_000u64 {
        c.add(i % 3);
    }
    println!("{}", c.get());
}
