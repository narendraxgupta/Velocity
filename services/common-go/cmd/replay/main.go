package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"

	eventstore "github.com/velocity/platform/services/common-go/eventstore"
)

func main() {
	brokers := flag.String("brokers", "localhost:9092", "comma-separated list of Kafka brokers")
	topic := flag.String("topic", "events", "topic to replay")
	flag.Parse()

	if *brokers == "" {
		fmt.Fprintln(os.Stderr, "brokers required")
		os.Exit(2)
	}

	store := eventstore.NewRedpandaEventStore([]string{*brokers})

	fmt.Printf("Replaying topic %s from brokers %s\n", *topic, *brokers)

	err := store.Replay(*topic, func(e *eventstore.Envelope) error {
		b, _ := json.MarshalIndent(e, "", "  ")
		log.Println(string(b))
		return nil
	})
	if err != nil {
		log.Fatalf("replay failed: %v", err)
	}
	fmt.Println("replay complete")
}
