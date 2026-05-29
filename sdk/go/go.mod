// =============================================================================
//  velocity-platform Go SDK
//
//  Public, importable surface for talking to a Velocity deployment.
//  Pinned at v0 until we ship 1.0; the API may change in minor releases
//  during that time.
// =============================================================================
module github.com/velocity/platform/sdk/go

go 1.22

// Resolve proto stubs locally during dev. CI substitutes a tagged
// version of proto/gen/go when publishing.
replace github.com/velocity/platform/proto/gen/go => ../../proto/gen/go
