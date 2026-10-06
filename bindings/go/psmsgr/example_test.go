// SPDX-License-Identifier: Apache-2.0

package psmsgr_test

import (
	"context"
	"fmt"
	"log"
	"os"
	"time"

	"github.com/ps-solucoes/ps-msgr/bindings/go/psmsgr"
)

// MotorStatus mirrors the C struct motor_status: a uint64_t and three
// floats, 24 bytes. The padding field makes it 24 bytes on 32-bit ARM too,
// where Go aligns a uint64 to 4 bytes but C to 8.
type MotorStatus struct {
	Sequence     uint64
	SpeedRpm     float32
	CurrentA     float32
	TemperatureC float32
	_            uint32
}

const MotorStatusV1 = 0x0001_0001 // schema 1, version 1 (payload type)

func Example() {
	dir, err := os.MkdirTemp("", "psmsgr-example-") // "" would mean /dev/shm
	if err != nil {
		log.Fatal(err)
	}
	defer os.RemoveAll(dir)

	// The reader may start before the writer.
	r, err := psmsgr.OpenReader("motor", dir)
	if err != nil {
		log.Fatal(err)
	}
	defer r.Close()

	w, err := psmsgr.OpenWriter("motor", 24, &psmsgr.WriterOptions{PayloadType: MotorStatusV1, Dir: dir})
	if err != nil {
		log.Fatal(err)
	}
	defer w.Close()
	status := MotorStatus{Sequence: 1, SpeedRpm: 1500, CurrentA: 2.5, TemperatureC: 41}
	if _, err := psmsgr.PublishValue(w, &status); err != nil {
		log.Fatal(err)
	}

	var seen uint32
	changed, err := r.Wait(context.Background(), seen, 500*time.Millisecond)
	if err != nil || !changed {
		log.Fatal("no value: ", err)
	}
	var got MotorStatus
	info, ok, err := psmsgr.ReadValue(r, &got)
	if err != nil || !ok {
		log.Fatal("no value: ", err)
	}
	if info.Attached {
		// A new channel file: check what it carries.
		desc, _, err := r.Describe()
		if err != nil || desc.PayloadType != MotorStatusV1 {
			log.Fatal("unexpected payload type")
		}
	}
	seen = info.Generation
	fmt.Printf("seq=%d speed=%.1f rpm\n", got.Sequence, got.SpeedRpm)
	// Output: seq=1 speed=1500.0 rpm
}

// One goroutine follows a channel and takes commands, with a select.
func ExampleReader_WaitChan() {
	dir, err := os.MkdirTemp("", "psmsgr-example-")
	if err != nil {
		log.Fatal(err)
	}
	defer os.RemoveAll(dir)
	w, err := psmsgr.OpenWriter("motor", 24, &psmsgr.WriterOptions{PayloadType: MotorStatusV1, Dir: dir})
	if err != nil {
		log.Fatal(err)
	}
	defer w.Close()
	r, err := psmsgr.OpenReader("motor", dir)
	if err != nil {
		log.Fatal(err)
	}
	defer r.Close()

	quit := make(chan struct{})
	go func() {
		for seq := uint64(1); seq <= 3; seq++ {
			status := MotorStatus{Sequence: seq, SpeedRpm: 1500}
			psmsgr.PublishValue(w, &status)
			time.Sleep(10 * time.Millisecond)
		}
		close(quit)
	}()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	var seen uint32
	var last MotorStatus
	for {
		ch := r.WaitChan(ctx, seen, psmsgr.NoTimeout)
		select {
		case res := <-ch:
			if res.Err != nil {
				log.Fatal(res.Err)
			}
			seen = res.Generation
			if _, _, err := psmsgr.ReadValue(r, &last); err != nil {
				log.Fatal(err)
			}
		case <-quit:
			cancel()
			<-ch // the reader is ours again
			if _, _, err := psmsgr.ReadValue(r, &last); err != nil {
				log.Fatal(err)
			}
			fmt.Printf("last seq=%d\n", last.Sequence)
			return
		}
	}
	// Output: last seq=3
}
