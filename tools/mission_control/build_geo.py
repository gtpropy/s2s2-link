#!/usr/bin/env python3
"""Builds static/geo.json for the Mission Control map from Natural Earth (public domain, naturalearthdata.com):
land (1:50m), country borders (1:50m lines), country polygons (1:110m, for names), ocean / sea names (1:110m marine polygons).
Usage: build_geo.py <dir with ne_50m_land.geojson ne_50m_admin_0_boundary_lines_land.geojson ne_110m_admin_0_countries.geojson ne_110m_geography_marine_polys.geojson>
Rings are stored as flat [lon, lat, lon, lat, ...] arrays rounded to 2 decimals."""
import json
import os
import sys

src = sys.argv[1]
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static", "geo.json")


def load(name):
    with open(os.path.join(src, name)) as f:
        return json.load(f)["features"]


def flat(ring):
    o, last = [], None
    for x, y, *_ in ring:
        p = (round(x, 2), round(y, 2))
        if p != last:
            o += [p[0], p[1]]
            last = p
    return o


def polys(geom):
    if geom["type"] == "Polygon":
        return [[flat(r) for r in geom["coordinates"]]]
    if geom["type"] == "MultiPolygon":
        return [[flat(r) for r in p] for p in geom["coordinates"]]
    return []


def lines(geom):
    if geom["type"] == "LineString":
        return [flat(geom["coordinates"])]
    if geom["type"] == "MultiLineString":
        return [flat(l) for l in geom["coordinates"]]
    return []


geo = {"land": [p for f in load("ne_50m_land.geojson") for p in polys(f["geometry"])],
       "borders": [l for f in load("ne_50m_admin_0_boundary_lines_land.geojson") for l in lines(f["geometry"])],
       "countries": [{"n": f["properties"].get("NAME") or f["properties"].get("name"), "p": polys(f["geometry"])} for f in load("ne_110m_admin_0_countries.geojson")],
       "seas": [{"n": f["properties"].get("name"), "p": polys(f["geometry"])} for f in load("ne_110m_geography_marine_polys.geojson")]}
with open(out, "w") as f:
    json.dump(geo, f, separators=(",", ":"))
print(out, os.path.getsize(out), "bytes;", len(geo["land"]), "land polygons,", len(geo["borders"]), "border lines,", len(geo["countries"]), "countries,", len(geo["seas"]), "seas")
