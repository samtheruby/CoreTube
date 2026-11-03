# SPDX-License-Identifier: BSD-3-Clause
"""Forward an opaque DRM provisioning request to Google's HTTPS endpoint."""

import http.client
import json
from urllib.parse import urlsplit


def request_provision(url, request):
    endpoint = urlsplit(url)
    if (
        endpoint.scheme != "https"
        or endpoint.hostname != "www.googleapis.com"
        or endpoint.port not in (None, 443)
        or endpoint.username is not None
        or endpoint.password is not None
        or endpoint.fragment
        or endpoint.path != "/certificateprovisioning/v1/devicecertificates/create"
    ):
        raise ValueError("Unexpected provisioning endpoint")
    body = json.dumps({"signedRequest": request.decode("utf-8")}).encode("utf-8")
    path = endpoint.path + ("?" + endpoint.query if endpoint.query else "")
    return _post(endpoint.hostname, path, body, "application/json")


def _post(host, path, body, content_type):
    connection = http.client.HTTPSConnection(host, timeout=15)
    try:
        connection.request("POST", path, body, {"Content-Type": content_type})
        response = connection.getresponse()
        if response.status != 200:
            raise RuntimeError(f"DRM server returned HTTP {response.status}")
        data = response.read(1024 * 1024 + 1)
        if not data or len(data) > 1024 * 1024:
            raise ValueError("Invalid DRM response size")
        return data
    finally:
        connection.close()
