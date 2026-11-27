$main :: proc() i32 {

	var %set = [10] {}

	for (var %i = u64 0; %i < 10; %i = (%i + 1)) {

		var %x = i32 0i32
		var %y = i32 {}

		if (%i == 3) continue
		else if (%i == 9) break

		*u8 (%set + %i) = 1u8
	}
	if ((*u8 (%set + 3) or *u8 (%set + 9)) or !*u8 (%set + 5))
		ret 1i32
}
