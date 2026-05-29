package injector

import "os"

// lookupEnv exists as a tiny wrapper so the rest of the package can use a
// uniform `(value, ok)` style without pulling `os` into the surface that
// reads as "intent": "this string came from configuration, not a literal".
func lookupEnv(key string) (string, bool) {
	v := os.Getenv(key)
	if v == "" {
		return "", false
	}
	return v, true
}
