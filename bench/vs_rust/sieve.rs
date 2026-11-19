// The sieve of Eratosthenes: indexing into a large list.
fn sieve(n: usize) -> usize {
    let mut flags = vec![true; n + 1];
    let mut count = 0;
    let mut i = 2;
    while i <= n {
        if flags[i] {
            count += 1;
            let mut j = i * i;
            while j <= n {
                flags[j] = false;
                j += i;
            }
        }
        i += 1;
    }
    count
}
fn main() {
    println!("{}", sieve(5_000_000));
}
