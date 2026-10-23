#pragma once

class DartApp;

// Structured export of the recovered application model (libraries, classes,
// fields, functions and closure layout) as JSON. Written only with -j/--json
// so the existing textual outputs stay unchanged.
class JsonDumper
{
public:
	explicit JsonDumper(DartApp& app) : app(app) {}

	// Write one document. With split=true the main file becomes an index and each
	// library is written to "<filename>.d/" so consumers can diff a single library.
	void Write(const char* filename, bool split = false);

private:
	DartApp& app;
};
