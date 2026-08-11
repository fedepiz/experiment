package chunk

////////////////////////////////
//~ fp: Chunk List
//
// A list of arrays of N elements. A push writes into the last chunk, and takes
// a new chunk from the allocator when that one is full. No element therefore
// moves after its push: a pointer into the list stays correct, and a push
// frees nothing, which suits an arena.
//
// The cost is the random access, which the list does not give. A caller walks
// the list from the front, with the iterator below, or chunk by chunk where it
// consumes whole runs, as an upload to the GPU does.

Chunk :: struct($T: typeid, $N: int) {
	next:  ^Chunk(T, N),
	count: int,
	items: [N]T,
}

List :: struct($T: typeid, $N: int) {
	first: ^Chunk(T, N),
	last:  ^Chunk(T, N),
	count: int,
}

// It gives the pointer to the element inside the list, so a caller can fill
// the members of a large element in place.
push :: proc(list: ^List($T, $N), value: T, allocator := context.allocator) -> ^T {
	c := list.last
	if c == nil || c.count >= N {
		c = new(Chunk(T, N), allocator)
		if list.first == nil {
			list.first = c
			list.last = c
		} else {
			list.last.next = c
			list.last = c
		}
	}
	elem := &c.items[c.count]
	elem^ = value
	c.count += 1
	list.count += 1
	return elem
}

// the first element, which is nil for an empty list
front :: proc(list: List($T, $N)) -> ^T {
	if list.first == nil {return nil}
	return &list.first.items[0]
}

////////////////////////////////
//~ fp: Iteration
//
//   it := chunk.iterator(list)
//   for elem in chunk.iterate(&it) { ... }

Iterator :: struct($T: typeid, $N: int) {
	chunk: ^Chunk(T, N),
	index: int,
}

iterator :: proc(list: List($T, $N)) -> Iterator(T, N) {
	return {list.first, 0}
}

iterate :: proc(it: ^Iterator($T, $N)) -> (elem: ^T, ok: bool) {
	for it.chunk != nil && it.index >= it.chunk.count {
		it.chunk = it.chunk.next
		it.index = 0
	}
	if it.chunk == nil {return nil, false}
	elem = &it.chunk.items[it.index]
	it.index += 1
	return elem, true
}
