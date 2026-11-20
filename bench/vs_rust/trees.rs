// Binary trees: building and walking many small structures.
enum Tree {
    Leaf,
    Node(Box<Tree>, Box<Tree>),
}
fn make(d: u32) -> Tree {
    if d == 0 { Tree::Leaf } else { Tree::Node(Box::new(make(d - 1)), Box::new(make(d - 1))) }
}
fn check(t: &Tree) -> u64 {
    match t {
        Tree::Leaf => 1,
        Tree::Node(l, r) => 1 + check(l) + check(r),
    }
}
fn main() {
    let max = 14;
    let mut depth = 4;
    while depth <= max {
        let iterations: u64 = 1 << (max - depth + 4);
        let mut nodes = 0;
        for _ in 0..iterations {
            nodes += check(&make(depth));
        }
        println!("{} {} {}", iterations, depth, nodes);
        depth += 2;
    }
}
