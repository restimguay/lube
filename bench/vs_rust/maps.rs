// A hash map with string keys: inserting, then looking up.
use std::collections::HashMap;
fn main() {
    let mut m: HashMap<String, u64> = HashMap::new();
    for i in 0..500_000u64 {
        m.insert(format!("k{}", i), i);
    }
    let mut total = 0;
    for i in 0..500_000u64 {
        total += m[&format!("k{}", i)];
    }
    println!("{} {}", m.len(), total);
}
