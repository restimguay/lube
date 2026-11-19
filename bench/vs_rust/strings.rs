// Strings: formatting, joining and splitting.
fn main() {
    let mut parts: Vec<String> = Vec::new();
    for i in 0..300_000 {
        parts.push(format!("item {}", i));
    }
    let joined = parts.join(",");
    let mut count = 0;
    for p in joined.split(',') {
        if p.ends_with('7') {
            count += 1;
        }
    }
    println!("{} {}", joined.len(), count);
}
