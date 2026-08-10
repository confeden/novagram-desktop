/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Platform {

class Integration {
public:
	virtual void init() {
	}

	// The decoy renames the application in place, without a restart. Anything
	// the shell caches under the old name — the Windows jump list in
	// particular — has to be rebuilt once it is armed. A no-op where there is
	// no such cache.
	virtual void refreshCustomJumpList() {
	}

	virtual ~Integration();

	[[nodiscard]] static std::unique_ptr<Integration> Create();
	[[nodiscard]] static Integration &Instance();
};

} // namespace Platform
