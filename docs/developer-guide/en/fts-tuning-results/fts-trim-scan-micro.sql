SET NAMES utf8mb4;
SET @trim_input = REPEAT('!!! ??? ___ --- ... +++ ', 200);
SET @trim_start = NOW(6);
SELECT BENCHMARK(300, TOKENIZE(@trim_input, 'beng'));
SELECT TIMESTAMPDIFF(MICROSECOND, @trim_start, NOW(6)) AS punctuation_us;
SET @trim_input = REPEAT('a 7 Z ! ? _ - ', 200);
SET @trim_start = NOW(6);
SELECT BENCHMARK(300, TOKENIZE(@trim_input, 'beng'));
SELECT TIMESTAMPDIFF(MICROSECOND, @trim_start, NOW(6)) AS single_byte_us;
