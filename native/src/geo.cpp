// Issue #22: geo-reference and geo projections of the map.
//
// CARLA ue5-dev converts between locations and geo locations with the map's
// geom::GeoProjection (or one given by the caller). CARLA 0.10.0 has no
// projections: only the geo-reference's Mercator approximation
// (GeoLocation::Transform), and no inverse.
#include "internal.hpp"

using namespace tsc;

namespace {

#ifdef TSC_HAS_GEO_PROJECTION
namespace cg = carla::geom;

cg::Ellipsoid ellipsoid_of(const tsc_geo_projection_t &p) {
  return cg::Ellipsoid(p.ellipsoid_a, p.ellipsoid_f_inv);
}

cg::GeoProjection projection_to_carla(const tsc_geo_projection_t &p) {
  switch (p.type) {
    case TSC_GEO_PROJECTION_TM:
      return cg::GeoProjection::Make(
          cg::TransverseMercatorParams(p.lat_0, p.lon_0, p.k, p.x_0, p.y_0, ellipsoid_of(p)));
    case TSC_GEO_PROJECTION_UTM: {
      cg::UniversalTransverseMercatorParams utm(p.utm_zone, p.utm_north != 0, ellipsoid_of(p));
      if (p.utm_has_offset != 0) {
        cg::OffsetTransform offset;
        offset.offset_x = p.offset_x;
        offset.offset_y = p.offset_y;
        offset.offset_z = p.offset_z;
        offset.offset_cos_h = p.offset_cos_h;
        offset.offset_sin_h = p.offset_sin_h;
        utm.offset = offset;
      }
      return cg::GeoProjection::Make(std::move(utm));
    }
    case TSC_GEO_PROJECTION_WEB_MERC:
      return cg::GeoProjection::Make(cg::WebMercatorParams(ellipsoid_of(p)));
    case TSC_GEO_PROJECTION_LCC2SP:
      return cg::GeoProjection::Make(cg::LambertConformalConicParams(
          p.lat_0, p.lat_1, p.lat_2, p.lon_0, p.x_0, p.y_0, ellipsoid_of(p)));
    default:
      fail(TSC_INVALID_ARGUMENT, "invalid geo projection type " + std::to_string(p.type));
  }
}

void set_ellipsoid(tsc_geo_projection_t &out, const cg::Ellipsoid &e) {
  out.ellipsoid_a = e.a;
  out.ellipsoid_f_inv = e.f_inv;
}

// One overload per alternative of geom::ProjectionParams.
void fill(tsc_geo_projection_t &out, const cg::TransverseMercatorParams &p) {
  out.type = TSC_GEO_PROJECTION_TM;
  out.lat_0 = p.lat_0;
  out.lon_0 = p.lon_0;
  out.k = p.k;
  out.x_0 = p.x_0;
  out.y_0 = p.y_0;
  set_ellipsoid(out, p.ellps);
}

void fill(tsc_geo_projection_t &out, const cg::UniversalTransverseMercatorParams &p) {
  out.type = TSC_GEO_PROJECTION_UTM;
  out.utm_zone = p.zone;
  out.utm_north = p.north ? 1 : 0;
  set_ellipsoid(out, p.ellps);
  if (p.offset) {
    out.utm_has_offset = 1;
    out.offset_x = p.offset->offset_x;
    out.offset_y = p.offset->offset_y;
    out.offset_z = p.offset->offset_z;
    out.offset_cos_h = p.offset->offset_cos_h;
    out.offset_sin_h = p.offset->offset_sin_h;
  }
}

void fill(tsc_geo_projection_t &out, const cg::WebMercatorParams &p) {
  out.type = TSC_GEO_PROJECTION_WEB_MERC;
  set_ellipsoid(out, p.ellps);
}

void fill(tsc_geo_projection_t &out, const cg::LambertConformalConicParams &p) {
  out.type = TSC_GEO_PROJECTION_LCC2SP;
  out.lat_0 = p.lat_0;
  out.lat_1 = p.lat_1;
  out.lat_2 = p.lat_2;
  out.lon_0 = p.lon_0;
  out.x_0 = p.x_0;
  out.y_0 = p.y_0;
  set_ellipsoid(out, p.ellps);
}

// The caller's projection, or the map's.
cg::GeoProjection projection_or_map(const tsc_map_t *map, const tsc_geo_projection_t *projection) {
  return projection != nullptr ? projection_to_carla(*projection) : map_of(map).GetGeoProjection();
}
#else
[[noreturn]] void no_projections(const char *what) {
  fail(TSC_ERROR, std::string(what) + " needs geo projections, which this LibCarla (" +
                      carla::version() + ") does not have; they are in CARLA ue5-dev");
}
#endif

}  // namespace

extern "C" {

tsc_status_t tsc_map_get_geoprojection(const tsc_map_t *map, tsc_geo_projection_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
#ifdef TSC_HAS_GEO_PROJECTION
    tsc_geo_projection_t r{};
    // visit by ADL: boost::variant2 in LibCarla, std::variant in the mock.
    visit([&](const auto &params) { fill(r, params); }, map_of(map).GetGeoProjection().params);
    *out = r;
#else
    map_of(map);
    no_projections("Map.get_geoprojection");
#endif
  });
}

tsc_status_t tsc_map_transform_to_geolocation(const tsc_map_t *map,
                                              const tsc_location_t *location,
                                              const tsc_geo_projection_t *projection,
                                              tsc_geo_location_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto loc = to_carla(*require_ptr(location, "location"));
#ifdef TSC_HAS_GEO_PROJECTION
    *out = from_carla(projection_or_map(map, projection).TransformToGeoLocation(loc));
#else
    const auto &m = map_of(map);
    if (projection != nullptr) no_projections("Map.transform_to_geolocation with a projection");
    *out = from_carla(m.GetGeoReference().Transform(loc));
#endif
  });
}

tsc_status_t tsc_map_geolocation_to_transform(const tsc_map_t *map,
                                              const tsc_geo_location_t *geolocation,
                                              const tsc_geo_projection_t *projection,
                                              tsc_location_t *out) {
  return TSC_GUARD({
    require_ptr(out, "out");
    const auto geo = to_carla(*require_ptr(geolocation, "geolocation"));
#ifdef TSC_HAS_GEO_PROJECTION
    *out = from_carla(projection_or_map(map, projection).GeoLocationToTransform(geo));
#else
    map_of(map);
    (void)geo;
    (void)projection;
    no_projections("Map.geolocation_to_transform");
#endif
  });
}

}  // extern "C"
