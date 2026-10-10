"""Smoke test against the independently maintained MCP Python SDK client."""

import asyncio
import sys

import httpx2
from mcp.client.session import ClientSession
from mcp.client.streamable_http import streamable_http_client
from mcp.client.subscriptions import listen
from mcp.shared.subscriptions import ResourceUpdated


async def main(endpoint, token):
    async with httpx2.AsyncClient(
        headers={"Authorization": f"Bearer {token}"}
    ) as http_client:
        async with streamable_http_client(
            endpoint, http_client=http_client, terminate_on_close=False
        ) as (read_stream, write_stream):
            async with ClientSession(read_stream, write_stream) as session:
                discovery = await session.discover()
                assert "2026-07-28" in discovery.supported_versions
                tools = await session.list_tools()
                assert any(tool.name == "browser.version" for tool in tools.tools)
                version = await session.call_tool("browser.version", {})
                assert not version.is_error
                resource = await session.read_resource(
                    "chromium-mcp://browser/version"
                )
                assert resource.contents
                events_uri = "chromium-mcp://browser/events"
                async with listen(
                    session, resource_subscriptions=[events_uri]
                ) as subscription:
                    assert events_uri in subscription.honored.resource_subscriptions
                    created = await session.call_tool("page.new", {})
                    assert not created.is_error
                    event = await asyncio.wait_for(anext(subscription), timeout=10)
                    assert isinstance(event, ResourceUpdated)
                    assert event.uri == events_uri


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1], sys.argv[2]))
