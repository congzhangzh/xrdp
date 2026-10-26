#!/usr/bin/python3
"""Checks a running compositor against what a wlup backend uses of it.

Mutter's D-Bus APIs and KWin's Wayland protocols are private to GNOME and
KDE and change between releases. This compares what the compositor in the
session offers with a contract file (mutter.contract, kwin.contract) of
what the backend calls, so that a change shows up in CI before a user
finds xrdp unable to connect. What the compositor offers is saved too
(D-Bus introspection XML, Wayland globals), to diff between releases.

Usage, as the session user (DBUS_SESSION_BUS_ADDRESS, WAYLAND_DISPLAY set):
  api-contract.py mutter <contract file> <output dir>
  api-contract.py kwin <contract file> <output dir>

Exit status: 0 if everything required is there, 1 if not. Missing
optional members are reported, but don't fail.
"""

import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

RD = "org.gnome.Mutter.RemoteDesktop"
SC = "org.gnome.Mutter.ScreenCast"


def read_contract(path):
    entries = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if line:
                entries.append(line)
    return entries


class Bus:
    """One connection to the session bus: Mutter ends a remote desktop
    session when the connection that made it closes, and lets only that
    connection use it"""

    def __init__(self):
        from gi.repository import Gio, GLib
        self.glib = GLib
        self.conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    def call(self, dest, path, iface, method, args=None, reply=None):
        result = self.conn.call_sync(
            dest, path, iface, method, args,
            self.glib.VariantType(reply) if reply else None,
            0, -1, None)
        return result.unpack() if result is not None else None

    def introspect(self, dest, path):
        return self.call(dest, path, "org.freedesktop.DBus.Introspectable",
                         "Introspect", None, "(s)")[0]

    def get(self, dest, path, iface, prop):
        return self.call(dest, path, "org.freedesktop.DBus.Properties",
                         "Get", self.glib.Variant("(ss)", (iface, prop)),
                         "(v)")[0]


def members(xml):
    """{interface: {(kind, name): (in signature, out signature)}}"""
    result = {}
    for iface in ET.fromstring(xml).findall("interface"):
        found = {}
        for node in iface:
            if node.tag == "property":
                found[("property", node.get("name"))] = ("-", node.get("type"))
            elif node.tag in ("method", "signal"):
                ins = "".join(a.get("type") for a in node.findall("arg")
                              if node.tag == "signal" or
                              a.get("direction", "in") == "in")
                outs = "".join(a.get("type") for a in node.findall("arg")
                               if node.tag == "method" and
                               a.get("direction") == "out")
                found[(node.tag, node.get("name"))] = (ins or "-", outs or "-")
        result[iface.get("name")] = found
    return result


def mutter(contract, out):
    bus = Bus()
    variant = bus.glib.Variant
    rd_root = "/org/gnome/Mutter/RemoteDesktop"
    sc_root = "/org/gnome/Mutter/ScreenCast"
    xml = {}
    xml["RemoteDesktop"] = bus.introspect(RD, rd_root)
    xml["ScreenCast"] = bus.introspect(SC, sc_root)
    # the client's monitor layout goes to GNOME through DisplayConfig
    xml["DisplayConfig"] = bus.introspect("org.gnome.Mutter.DisplayConfig",
                                          "/org/gnome/Mutter/DisplayConfig")
    # The session interfaces live on objects that exist only once a
    # session is made: make one, never start it, and stop it again
    rd_path = bus.call(RD, rd_root, RD, "CreateSession", None, "(o)")[0]
    try:
        xml["RemoteDesktop.Session"] = bus.introspect(RD, rd_path)
        session_id = bus.get(RD, rd_path, RD + ".Session", "SessionId")
        sc_path = bus.call(SC, sc_root, SC, "CreateSession",
                           variant("(a{sv})", ({
                               "remote-desktop-session-id":
                               variant("s", session_id)},)), "(o)")[0]
        xml["ScreenCast.Session"] = bus.introspect(SC, sc_path)
        stream_path = bus.call(SC, sc_path, SC + ".Session", "RecordVirtual",
                               variant("(a{sv})", ({},)), "(o)")[0]
        xml["ScreenCast.Stream"] = bus.introspect(SC, stream_path)
    finally:
        try:
            bus.call(RD, rd_path, RD + ".Session", "Stop")
        except Exception:  # an unstarted session may refuse it
            pass

    offered = {}
    for name, text in xml.items():
        with open(os.path.join(out, "api-mutter-%s.xml" % name), "w",
                  encoding="utf-8") as f:
            f.write(text)
        offered.update(members(text))
    for iface, path in ((RD, rd_root), (SC, sc_root)):
        print("INFO     %s.Version = %d" %
              (iface, bus.get(iface, path, iface, "Version")))

    failed = 0
    for need, iface, kind, name, ins, outs in contract:
        got = offered.get(iface, {}).get((kind, name))
        what = "%s %s %s.%s(%s) -> %s" % (need, kind, iface, name, ins, outs)
        if got is None:
            status = "MISSING"
        elif got != (ins, outs):
            status = "CHANGED"
            what += ", offered (%s) -> %s" % got
        else:
            status = "OK"
        if status != "OK" and need == "required":
            failed = 1
        print("%-8s %s" % (status, what))
    return failed


def kwin(contract, out):
    text = subprocess.run(["wayland-info"], check=True, capture_output=True,
                          text=True).stdout
    offered = {}
    for m in re.finditer(r"interface: '([^']+)',\s*version:\s*(\d+)", text):
        offered[m.group(1)] = max(offered.get(m.group(1), 0), int(m.group(2)))
    with open(os.path.join(out, "api-kwin-globals.txt"), "w",
              encoding="utf-8") as f:
        for name in sorted(offered):
            f.write("%s %d\n" % (name, offered[name]))

    failed = 0
    for need, kind, name, version in contract:
        got = offered.get(name, 0)
        what = "%s %s %s >= %s" % (need, kind, name, version)
        if got == 0:
            status = "MISSING"
        elif got < int(version):
            status = "OLD"
            what += ", offered %d" % got
        else:
            status = "OK"
            what += " (offered %d)" % got
        if status != "OK" and need == "required":
            failed = 1
        print("%-8s %s" % (status, what))
    return failed


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in ("mutter", "kwin"):
        sys.exit(__doc__)
    contract = read_contract(sys.argv[2])
    os.makedirs(sys.argv[3], exist_ok=True)
    check = mutter if sys.argv[1] == "mutter" else kwin
    sys.exit(check(contract, sys.argv[3]))


if __name__ == "__main__":
    main()
