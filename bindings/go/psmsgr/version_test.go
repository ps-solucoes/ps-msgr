// SPDX-License-Identifier: Apache-2.0

package psmsgr

import (
	"strings"
	"testing"
)

func TestCheckVersion(t *testing.T) {
	if err := checkLoaded(); err != nil {
		t.Fatal(err)
	}
	for _, v := range []uint32{1<<16 | 1<<8 | 0, 1<<16 | 7<<8 | 3} {
		if err := checkVersion(v); err != nil {
			t.Errorf("%#x: %v", v, err)
		}
	}
	for _, v := range []uint32{1<<16 | 0<<8 | 9, 0<<16 | 9<<8, 2 << 16} {
		err := checkVersion(v)
		e, ok := err.(*Error)
		if !ok || e.Code != ErrNotSup || !strings.Contains(e.Error(), "needs libpsmsgr 1.1 or a later 1.x") {
			t.Errorf("%#x: %v", v, err)
		}
	}
}
