#pragma once

class DartApp;

// Structured export of the recovered application model (libraries, classes,
// fields, functions and closure layout) as JSON. Written only with -j/--json
// so the existing textual outputs stay unchanged.
class JsonDumper
{
public:
	explicit JsonDumper(DartApp& app) : app(app) {}

	void Write(const char* filename);

private:
	DartApp& app;
};
