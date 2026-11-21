$main :: proc() i32 {

	if (1 == 2) ret 1i32

	if (1 != 1) ret 2i32

	if (2 < 1) ret 3i32

	if (1 > 2) ret 4i32

	if (0 and 0) ret 5i32

	if (0 and 1) ret 6i32

	if (1 and 0) ret 7i32

	if (0 or 0) ret 8i32

	if (!(1 and 1)) ret 9i32

	if (!(1 or 0)) ret 10i32

	if (!(0 or 1)) ret 11i32

	if (!(1 or 1)) ret 12i32

	if (1 == 2) ret 13i32
	else ret 0i32

	ret 14i32
}
