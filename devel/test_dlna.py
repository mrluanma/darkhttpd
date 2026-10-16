#!/usr/bin/env python3
# This is run by the "run-tests" script against a darkhttpd started with
# --dlna. It checks the DLNA/UPnP HTTP endpoints and (best effort) SSDP
# discovery.
import os
import socket
import unittest

WWWROOT = "tmp.httpd.tests"
PORT = 12346
ADDR = "127.0.0.1"

MEDIA = "dlna_probe.mp4"
SUBDIR = "dlna_probe_dir"
NESTED = "nested.mkv"


def http_raw(request):
    """Send a raw request and return the full response bytes."""
    s = socket.socket()
    s.settimeout(5)
    s.connect((ADDR, PORT))
    s.sendall(request)
    chunks = []
    while True:
        try:
            r = s.recv(65536)
        except socket.timeout:
            break
        if not r:
            break
        chunks.append(r)
    s.close()
    return b"".join(chunks)


def http_get(path):
    return http_raw(
        ("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
         % (path, ADDR)).encode())


def soap_post(path, service, action, body):
    envelope = (
        '<?xml version="1.0" encoding="utf-8"?>'
        '<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/">'
        '<s:Body>%s</s:Body></s:Envelope>' % body).encode()
    req = (
        b"POST " + path.encode() + b" HTTP/1.1\r\n"
        b"Host: " + ADDR.encode() + b"\r\n"
        b'Content-Type: text/xml; charset="utf-8"\r\n'
        b'SOAPAction: "urn:schemas-upnp-org:service:' +
        service.encode() + b":1#" + action.encode() + b'"\r\n'
        b"Content-Length: " + str(len(envelope)).encode() + b"\r\n"
        b"Connection: close\r\n\r\n" + envelope)
    return http_raw(req)


def status_of(resp):
    return resp.split(b"\r\n", 1)[0]


def xml_unescape(data):
    for ent, ch in ((b"&lt;", b"<"), (b"&gt;", b">"), (b"&quot;", b'"'),
                    (b"&apos;", b"'"), (b"&amp;", b"&")):
        data = data.replace(ent, ch)
    return data


class TestDlna(unittest.TestCase):
    def setUp(self):
        self.files = []
        with open(os.path.join(WWWROOT, MEDIA), "wb") as f:
            f.write(b"fake video data")
        self.files.append(MEDIA)
        os.mkdir(os.path.join(WWWROOT, SUBDIR))
        with open(os.path.join(WWWROOT, SUBDIR, NESTED), "wb") as f:
            f.write(b"fake nested video")
        self.files.append(SUBDIR)

    def tearDown(self):
        for name in self.files:
            path = os.path.join(WWWROOT, name)
            if os.path.isdir(path):
                for entry in os.listdir(path):
                    os.unlink(os.path.join(path, entry))
                os.rmdir(path)
            elif os.path.exists(path):
                os.unlink(path)

    def test_device_description(self):
        resp = http_get("/rootDesc.xml")
        self.assertIn(b"200 OK", status_of(resp))
        self.assertIn(b"<friendlyName>", resp)
        self.assertIn(b"urn:schemas-upnp-org:device:MediaServer:1", resp)
        self.assertIn(b"urn:schemas-upnp-org:service:ContentDirectory:1", resp)
        self.assertIn(b"urn:schemas-upnp-org:service:ConnectionManager:1", resp)
        self.assertIn(b"<UDN>uuid:", resp)

    def test_scpd(self):
        for service, action in (
                ("ContentDirectory", b"Browse"),
                ("ConnectionManager", b"GetProtocolInfo")):
            resp = http_get("/%s/scpd.xml" % service)
            self.assertIn(b"200 OK", status_of(resp))
            self.assertIn(action, resp)

    def test_browse_root(self):
        resp = soap_post(
            "/ContentDirectory/control", "ContentDirectory", "Browse",
            "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:"
            "ContentDirectory:1\"><ObjectID>0</ObjectID>"
            "<BrowseFlag>BrowseDirectChildren</BrowseFlag><Filter>*</Filter>"
            "<StartingIndex>0</StartingIndex><RequestedCount>0</RequestedCount>"
            "<SortCriteria></SortCriteria></u:Browse>")
        self.assertIn(b"200 OK", status_of(resp))
        self.assertIn(b"BrowseResponse", resp)
        self.assertIn(b"NumberReturned>", resp)
        # The probe file and directory must be present as DIDL items.
        didl = xml_unescape(resp)
        self.assertIn(b"object.item.videoItem", didl)
        self.assertIn(b"object.container.storageFolder", didl)
        self.assertIn(MEDIA.encode(), didl)
        self.assertIn(SUBDIR.encode(), didl)

    def test_browse_metadata(self):
        resp = soap_post(
            "/ContentDirectory/control", "ContentDirectory", "Browse",
            "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:"
            "ContentDirectory:1\"><ObjectID>/" + MEDIA.encode().decode() +
            "</ObjectID><BrowseFlag>BrowseMetadata</BrowseFlag><Filter>*</Filter>"
            "<StartingIndex>0</StartingIndex><RequestedCount>1</RequestedCount>"
            "<SortCriteria></SortCriteria></u:Browse>")
        self.assertIn(b"200 OK", status_of(resp))
        didl = xml_unescape(resp)
        self.assertIn(b'protocolInfo="http-get:*:video/mp4:', didl)
        self.assertIn(b"object.item.videoItem", didl)

    def test_browse_subdir(self):
        resp = soap_post(
            "/ContentDirectory/control", "ContentDirectory", "Browse",
            "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:"
            "ContentDirectory:1\"><ObjectID>/" + SUBDIR.encode().decode() +
            "</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
            "<Filter>*</Filter><StartingIndex>0</StartingIndex>"
            "<RequestedCount>0</RequestedCount>"
            "<SortCriteria></SortCriteria></u:Browse>")
        self.assertIn(b"200 OK", status_of(resp))
        self.assertIn(NESTED.encode(), xml_unescape(resp))

    def test_protocol_info(self):
        resp = soap_post(
            "/ConnectionManager/control", "ConnectionManager",
            "GetProtocolInfo",
            "<u:GetProtocolInfo xmlns:u=\"urn:schemas-upnp-org:service:"
            "ConnectionManager:1\"/>")
        self.assertIn(b"200 OK", status_of(resp))
        self.assertIn(b"GetProtocolInfoResponse", resp)
        self.assertIn(b"http-get:*:video/mp4:", resp)

    def test_media_dlna_headers(self):
        resp = http_get("/" + MEDIA)
        self.assertIn(b"200 OK", status_of(resp))
        self.assertIn(b"Content-Type: video/mp4", resp)
        self.assertIn(b"transferMode.dlna.org: Streaming", resp)
        self.assertIn(b"contentFeatures.dlna.org:", resp)

    def test_unknown_action_faults(self):
        resp = soap_post(
            "/ContentDirectory/control", "ContentDirectory", "NoSuchAction",
            "<u:NoSuchAction xmlns:u=\"urn:schemas-upnp-org:service:"
            "ContentDirectory:1\"/>")
        self.assertIn(b"500", status_of(resp))
        self.assertIn(b"Fault", resp)

    def test_ssdp(self):
        """Best-effort: SSDP may be unavailable in restricted environments."""
        msg = (b"M-SEARCH * HTTP/1.1\r\n"
               b"HOST: 239.255.255.250:1900\r\n"
               b'MAN: "ssdp:discover"\r\n'
               b"MX: 1\r\n"
               b"ST: urn:schemas-upnp-org:device:MediaServer:1\r\n\r\n")
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(2)
        try:
            s.sendto(msg, (ADDR, 1900))
            data, _ = s.recvfrom(4096)
        except (socket.timeout, OSError) as e:
            print("note: SSDP not reachable (%s); skipping" % e)
            return
        finally:
            s.close()
        self.assertIn(b"200 OK", data)
        self.assertIn(b"ST: urn:schemas-upnp-org:device:MediaServer:1", data)
        self.assertIn(b"LOCATION: http://", data)
        self.assertIn(b"USN: uuid:", data)


if __name__ == "__main__":
    unittest.main()
