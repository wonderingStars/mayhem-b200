// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.

package web

import (
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
)

// --- JSON plumbing ----------------------------------------------------------

type errorResponse struct {
	Error string `json:"error"`
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, err error) {
	writeJSON(w, status, errorResponse{Error: err.Error()})
}

// statusForError maps a Backend/Device error to an HTTP status. Sentinel
// errors from types.go get specific statuses; anything else (a malformed
// request, an ad hoc hardware error) is a 400.
func statusForError(err error) int {
	switch {
	case errors.Is(err, ErrNoDevice):
		return http.StatusConflict
	case errors.Is(err, ErrDeviceInUse):
		return http.StatusConflict
	case errors.Is(err, ErrUnsupported):
		return http.StatusNotImplemented
	case errors.Is(err, ErrNotStreaming):
		return http.StatusConflict
	default:
		return http.StatusBadRequest
	}
}

// decodeJSON decodes the request body into v. A missing/empty body is not an
// error (v is left at its zero value) so that POSTs like /api/rx/stop, which
// take no arguments, don't have to special-case an empty body.
func decodeJSON(r *http.Request, v any) error {
	if r.Body == nil {
		return nil
	}
	dec := json.NewDecoder(r.Body)
	if err := dec.Decode(v); err != nil {
		if errors.Is(err, io.EOF) {
			return nil
		}
		return fmt.Errorf("invalid request body: %w", err)
	}
	return nil
}

// --- Device discovery & lifecycle -------------------------------------------

func (s *Server) handleListDevices(w http.ResponseWriter, r *http.Request) {
	devices, err := s.backend.ListDevices(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	if devices == nil {
		devices = []DeviceInfo{}
	}
	writeJSON(w, http.StatusOK, struct {
		Devices []DeviceInfo `json:"devices"`
	}{devices})
}

type openDeviceBody struct {
	Args string `json:"args"`
}

func (s *Server) handleOpenDevice(w http.ResponseWriter, r *http.Request) {
	var body openDeviceBody
	if err := decodeJSON(r, &body); err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	dev, err := s.backend.OpenDevice(r.Context(), body.Args)
	if err != nil {
		writeError(w, statusForError(err), err)
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Caps Caps `json:"caps"`
	}{dev.Caps()})
}

func (s *Server) handleCloseDevice(w http.ResponseWriter, r *http.Request) {
	if err := s.backend.CloseDevice(); err != nil {
		writeError(w, statusForError(err), err)
		return
	}
	writeJSON(w, http.StatusOK, struct{}{})
}

func (s *Server) handleCaps(w http.ResponseWriter, r *http.Request) {
	dev, ok := s.backend.CurrentDevice()
	if !ok {
		writeError(w, statusForError(ErrNoDevice), ErrNoDevice)
		return
	}
	writeJSON(w, http.StatusOK, struct {
		Caps Caps `json:"caps"`
	}{dev.Caps()})
}

func (s *Server) handleState(w http.ResponseWriter, r *http.Request) {
	dev, ok := s.backend.CurrentDevice()
	if !ok {
		writeError(w, statusForError(ErrNoDevice), ErrNoDevice)
		return
	}
	writeJSON(w, http.StatusOK, dev.State())
}

func (s *Server) handleStats(w http.ResponseWriter, r *http.Request) {
	dev, ok := s.backend.CurrentDevice()
	if !ok {
		writeError(w, statusForError(ErrNoDevice), ErrNoDevice)
		return
	}
	writeJSON(w, http.StatusOK, dev.Stats())
}

func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	st := ServerStatus{
		UptimeSeconds: s.backend.Uptime().Seconds(),
		Sessions:      s.backend.Sessions(),
	}
	if st.Sessions == nil {
		st.Sessions = []SessionInfo{}
	}
	if dev, ok := s.backend.CurrentDevice(); ok {
		st.DeviceOpen = true
		stats := dev.Stats()
		st.Stats = &stats
	}
	writeJSON(w, http.StatusOK, st)
}

// --- Generic setters ---------------------------------------------------------
//
// Every one of these mirrors a PROTOCOL.md §2.1 setter 1:1: decode the
// request, call the device, and return exactly what the device says was
// accepted — never the requested value. See the Device interface doc in
// types.go for why that indirection matters.

type hzBody struct {
	Hz float64 `json:"hz"`
}
type hzResult struct {
	Hz float64 `json:"hz"`
}

func (s *Server) handleHzSetter(set func(Device, float64) (float64, error)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		dev, ok := s.backend.CurrentDevice()
		if !ok {
			writeError(w, statusForError(ErrNoDevice), ErrNoDevice)
			return
		}
		var body hzBody
		if err := decodeJSON(r, &body); err != nil {
			writeError(w, http.StatusBadRequest, err)
			return
		}
		actual, err := set(dev, body.Hz)
		if err != nil {
			writeError(w, statusForError(err), err)
			return
		}
		writeJSON(w, http.StatusOK, hzResult{Hz: actual})
	}
}

type dbBody struct {
	Db float64 `json:"db"`
}
type dbResult struct {
	Db float64 `json:"db"`
}

func (s *Server) handleDbSetter(set func(Device, float64) (float64, error)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		dev, ok := s.backend.CurrentDevice()
		if !ok {
			writeError(w, statusForError(ErrNoDevice), ErrNoDevice)
			return
		}
		var body dbBody
		if err := decodeJSON(r, &body); err != nil {
			writeError(w, http.StatusBadRequest, err)
			return
		}
		actual, err := set(dev, body.Db)
		if err != nil {
			writeError(w, statusForError(err), err)
			return
		}
		writeJSON(w, http.StatusOK, dbResult{Db: actual})
	}
}

type nameBody struct {
	Name string `json:"name"`
}
type nameResult struct {
	Name string `json:"name"`
}

func (s *Server) handleNameSetter(set func(Device, string) (string, error)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		dev, ok := s.backend.CurrentDevice()
		if !ok {
			writeError(w, statusForError(ErrNoDevice), ErrNoDevice)
			return
		}
		var body nameBody
		if err := decodeJSON(r, &body); err != nil {
			writeError(w, http.StatusBadRequest, err)
			return
		}
		actual, err := set(dev, body.Name)
		if err != nil {
			writeError(w, statusForError(err), err)
			return
		}
		writeJSON(w, http.StatusOK, nameResult{Name: actual})
	}
}

type boolBody struct {
	On bool `json:"on"`
}
type boolResult struct {
	On bool `json:"on"`
}

func (s *Server) handleBoolSetter(set func(Device, bool) (bool, error)) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		dev, ok := s.backend.CurrentDevice()
		if !ok {
			writeError(w, statusForError(ErrNoDevice), ErrNoDevice)
			return
		}
		var body boolBody
		if err := decodeJSON(r, &body); err != nil {
			writeError(w, http.StatusBadRequest, err)
			return
		}
		actual, err := set(dev, body.On)
		if err != nil {
			writeError(w, statusForError(err), err)
			return
		}
		writeJSON(w, http.StatusOK, boolResult{On: actual})
	}
}

// --- Streaming control --------------------------------------------------------

type startRxBody struct {
	Format string `json:"format"`
}
type startRxResult struct {
	Format string `json:"format"`
}

func (s *Server) handleStartRx(w http.ResponseWriter, r *http.Request) {
	dev, ok := s.backend.CurrentDevice()
	if !ok {
		writeError(w, statusForError(ErrNoDevice), ErrNoDevice)
		return
	}
	var body startRxBody
	if err := decodeJSON(r, &body); err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	if body.Format == "" {
		body.Format = "cf32"
	}
	format, err := dev.StartRx(body.Format)
	if err != nil {
		writeError(w, statusForError(err), err)
		return
	}
	writeJSON(w, http.StatusOK, startRxResult{Format: format})
}

func (s *Server) handleStopRx(w http.ResponseWriter, r *http.Request) {
	dev, ok := s.backend.CurrentDevice()
	if !ok {
		writeError(w, statusForError(ErrNoDevice), ErrNoDevice)
		return
	}
	if err := dev.StopRx(); err != nil {
		writeError(w, statusForError(err), err)
		return
	}
	writeJSON(w, http.StatusOK, struct{}{})
}
