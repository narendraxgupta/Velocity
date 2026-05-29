// Package log wraps zap with the same conventions used by the C++ services:
// a single named logger with a "service" field, sane defaults for the dev
// loop, level read from VELOCITY_LOG_LEVEL.
package log

import (
	"os"
	"strings"

	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"
)

// New constructs a sugared logger tagged with the given service name.
//
// The returned logger writes structured JSON to stderr in production builds
// and human-friendly console output when VELOCITY_LOG_FORMAT=console (the
// default for local dev when stderr is a TTY).
func New(service string) *zap.SugaredLogger {
	level := parseLevel(os.Getenv("VELOCITY_LOG_LEVEL"))
	format := strings.ToLower(strings.TrimSpace(os.Getenv("VELOCITY_LOG_FORMAT")))

	var cfg zap.Config
	if format == "console" || (format == "" && isTerminal(os.Stderr)) {
		cfg = zap.NewDevelopmentConfig()
		cfg.EncoderConfig.EncodeLevel = zapcore.CapitalColorLevelEncoder
		cfg.DisableStacktrace = true
	} else {
		cfg = zap.NewProductionConfig()
		cfg.EncoderConfig.TimeKey = "ts"
		cfg.EncoderConfig.MessageKey = "msg"
		cfg.EncoderConfig.LevelKey = "level"
		cfg.EncoderConfig.EncodeTime = zapcore.ISO8601TimeEncoder
	}
	cfg.Level = zap.NewAtomicLevelAt(level)

	base, err := cfg.Build(zap.AddCallerSkip(1))
	if err != nil {
		// We genuinely cannot continue without a logger; the very early
		// failure mode is intentional.
		panic("log.New: " + err.Error())
	}

	return base.With(zap.String("service", service)).Sugar()
}

func parseLevel(s string) zapcore.Level {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "debug":
		return zapcore.DebugLevel
	case "info", "":
		return zapcore.InfoLevel
	case "warn", "warning":
		return zapcore.WarnLevel
	case "error":
		return zapcore.ErrorLevel
	case "fatal":
		return zapcore.FatalLevel
	default:
		return zapcore.InfoLevel
	}
}

// isTerminal best-effort detects whether the given file is a TTY. We avoid
// pulling in golang.org/x/term to keep dependencies minimal — if the file's
// Stat().Mode() shows a ModeCharDevice, it's almost certainly a terminal.
func isTerminal(f *os.File) bool {
	if f == nil {
		return false
	}
	stat, err := f.Stat()
	if err != nil {
		return false
	}
	return (stat.Mode() & os.ModeCharDevice) != 0
}
