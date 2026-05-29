// =============================================================================
//  velocity/common-go
//
//  Shared Go library used by every Velocity backend service. Lives in
//  its own module so each service can pin it independently — useful
//  during early phases when the API is still in flux. Once it settles
//  we'll promote it to a workspace member.
//
//  Wire-in pattern (every consuming service):
//
//      require github.com/velocity/platform/services/common-go v0.0.0
//      replace github.com/velocity/platform/services/common-go => ../common-go
// =============================================================================

module github.com/velocity/platform/services/common-go

go 1.22

require (
	github.com/oklog/ulid/v2 v2.1.0
	github.com/twmb/franz-go v1.18.0
	google.golang.org/grpc v1.67.1
)

require (
	github.com/klauspost/compress v1.17.8 // indirect
	github.com/pierrec/lz4/v4 v4.1.21 // indirect
	github.com/twmb/franz-go/pkg/kmsg v1.9.0 // indirect
	golang.org/x/sys v0.24.0 // indirect
)
