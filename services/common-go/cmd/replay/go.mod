module github.com/velocity/services/common-go/cmd/replay

go 1.23

require (
	github.com/segmentio/kafka-go v0.4.51
	github.com/velocity/platform/services/common-go v0.0.0
)

require (
	github.com/klauspost/compress v1.17.8 // indirect
	github.com/pierrec/lz4/v4 v4.1.21 // indirect
)

replace github.com/velocity/platform/services/common-go => ../../
