/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "core/current_geo_location.h"

#include "base/platform/base_platform_info.h"
#include "data/raw/raw_countries_bounds.h"
#include "platform/platform_current_geo_location.h"

namespace Core {

GeoLocation ResolveCurrentCountryLocation() {
	const auto iso2 = Platform::SystemCountry().toUpper();
	const auto &bounds = Raw::CountryBounds();
	const auto i = bounds.find(iso2);
	if (i == end(bounds)) {
		return {
			.accuracy = GeoLocationAccuracy::Failed,
		};
	}
	return {
		.point = {
			(i->second.minLat + i->second.maxLat) / 2.,
			(i->second.minLon + i->second.maxLon) / 2.,
		},
		.bounds = {
			i->second.minLat,
			i->second.minLon,
			i->second.maxLat - i->second.minLat,
			i->second.maxLon - i->second.minLon,
		},
		.accuracy = GeoLocationAccuracy::Country,
	};
}

void ResolveCurrentGeoLocation(Fn<void(GeoLocation)> callback) {
	using namespace Platform;
	return ResolveCurrentExactLocation([done = std::move(callback)](
			GeoLocation result) {
		done(result.accuracy != GeoLocationAccuracy::Failed
			? result
			: ResolveCurrentCountryLocation());
	});
}

void ResolveLocationAddress(
		const GeoLocation &location,
		const QString &language,
		const QString &token,
		Fn<void(GeoAddress)> callback) {
	// NovaGram: reverse geocoding is off. Upstream asked api.mapbox.com for
	// a street name, sending the exact coordinates and the interface language
	// over a plain QNetworkAccessManager request - outside the MTProto proxy
	// and outside DoH, so a third party learned a precise position together
	// with the real address of whoever was looking at the map. The platform
	// resolver is not called either: on Windows it is an empty stub today,
	// but the file already includes Windows.Services.Maps, so an upstream
	// merge that fills it in would silently bring the same leak back.
	//
	// The caller is expected to fall back to printing the coordinates.
	callback({});
}

bool AreTheSame(const GeoLocation &a, const GeoLocation &b) {
	if (a.accuracy != GeoLocationAccuracy::Exact
		|| b.accuracy != GeoLocationAccuracy::Exact) {
		return false;
	}
	const auto normalize = [](float64 value) {
		value = std::fmod(value + 180., 360.);
		return (value + (value < 0. ? 360. : 0.)) - 180.;
	};
	constexpr auto kEpsilon = 0.0001;
	const auto lon1 = normalize(a.point.y());
	const auto lon2 = normalize(b.point.y());
	const auto diffLat = std::abs(a.point.x() - b.point.x());
	if (std::abs(a.point.x()) >= (90. - kEpsilon)
		|| std::abs(b.point.x()) >= (90. - kEpsilon)) {
		return diffLat <= kEpsilon;
	}
	auto diffLon = std::abs(lon1 - lon2);
	if (diffLon > 180.) {
		diffLon = 360. - diffLon;
	}

	return diffLat <= kEpsilon && diffLon <= kEpsilon;
}

} // namespace Core
