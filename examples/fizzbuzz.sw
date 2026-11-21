$fizzbuzz :: proc(%count u64) u64 {

	for (var %i = u64 1; %i < (%count + 1); %i = %i + 1) {

		var %j = u64 0
		if ((%i % 3) == 0) {
			$printf("Fizz"u8)i32
			%j = %j + 1
		}
		if ((%i % 5) == 0) {
			$printf("Buzz"u8)i32
			%j = %j + 1
		}
		if (!%j) {
			$printf("%d"u8, i32 %i)i32
		}
		$puts(""u8)i32
	}
}

$main :: proc() i32 {

	$fizzbuzz(100)i32
}
