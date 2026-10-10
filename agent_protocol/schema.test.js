import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import Ajv from 'ajv';

const schema = JSON.parse(
  await readFile(new URL('./browser_agent_protocol.schema.json', import.meta.url), 'utf8'),
);
const validate = new Ajv({ allErrors: true }).compile(schema);

const requestMeta = {
  'io.modelcontextprotocol/protocolVersion': '2026-07-28',
  'io.modelcontextprotocol/clientCapabilities': {},
};

const validMessages = [
  { jsonrpc: '2.0', id: 1, method: 'server/discover', params: { _meta: requestMeta } },
  { jsonrpc: '2.0', id: 'list-1', method: 'tools/list', params: { _meta: requestMeta } },
  {
    jsonrpc: '2.0', id: 'listen-1', method: 'subscriptions/listen',
    params: {
      notifications: { resourceSubscriptions: ['chromium-mcp://browser/events'] },
      _meta: requestMeta,
    },
  },
  { jsonrpc: '2.0', id: 'resources-1', method: 'resources/list', params: { _meta: requestMeta } },
  { jsonrpc: '2.0', id: 'resource-1', method: 'resources/read', params: { uri: 'chromium-mcp://browser/version', _meta: requestMeta } },
  {
    jsonrpc: '2.0',
    id: 7,
    method: 'tools/call',
    params: { name: 'page.evaluate', arguments: { expression: 'document.title' }, _meta: requestMeta },
  },
  { jsonrpc: '2.0', id: 7, result: { content: [{ type: 'text', text: 'Title' }] } },
  { jsonrpc: '2.0', id: 7, error: { code: -32602, message: 'Invalid params' } },
  { jsonrpc: '2.0', id: null, error: { code: -32700, message: 'Parse error.' } },
];

test('accepts MCP discovery, list, call, and response messages', () => {
  for (const message of validMessages) {
    assert.equal(validate(message), true, JSON.stringify(validate.errors));
  }
});

test('rejects malformed MCP requests', () => {
  assert.equal(validate({ jsonrpc: '1.0', id: 1, method: 'tools/list' }), false);
  assert.equal(validate({ jsonrpc: '2.0', id: 1, method: 'tools/call', params: {} }), false);
  assert.equal(validate({ jsonrpc: '2.0', id: 1, method: 'tools/call', params: { name: '', _meta: requestMeta } }), false);
  assert.equal(validate({
    jsonrpc: '2.0', id: 'listen-1', method: 'subscriptions/listen',
    params: { notifications: { resourceSubscriptions: [] }, _meta: {
      'io.modelcontextprotocol/protocolVersion': '2026-07-28',
    } },
  }), false);
});

test('listen filter and optional client identity match runtime validation', () => {
  for (const metadata of [
    { 'io.modelcontextprotocol/clientCapabilities': {} },
    { ...requestMeta, 'io.modelcontextprotocol/protocolVersion': '' },
    { ...requestMeta, 'io.modelcontextprotocol/protocolVersion': 7 },
    { 'io.modelcontextprotocol/protocolVersion': '2026-07-28' },
    { ...requestMeta, 'io.modelcontextprotocol/clientCapabilities': [] },
  ]) {
    assert.equal(validate({
      jsonrpc: '2.0', id: 1, method: 'tools/list', params: { _meta: metadata },
    }), false, JSON.stringify(metadata));
  }
  const listen = notifications => ({
    jsonrpc: '2.0', id: 'listen-1', method: 'subscriptions/listen',
    params: notifications === undefined
      ? { _meta: requestMeta }
      : { notifications, _meta: requestMeta },
  });
  for (const notifications of [
    undefined,
    'chromium-mcp://browser/events',
    { resourceSubscriptions: 'chromium-mcp://browser/events' },
    { resourceSubscriptions: [7] },
    { resourceSubscriptions: [''] },
    { toolsListChanged: 'true' },
    { unknownNotification: true },
  ]) {
    assert.equal(validate(listen(notifications)), false, JSON.stringify(notifications));
  }
  assert.equal(validate(listen({ resourceSubscriptions: ['chromium-mcp://browser/events'] })), true);
  for (const field of ['name', 'version']) {
    const identity = { name: 'test-client', version: '1' };
    identity[field] = '';
    assert.equal(validate({
      jsonrpc: '2.0', id: 1, method: 'tools/list',
      params: { _meta: { ...requestMeta, 'io.modelcontextprotocol/clientInfo': identity } },
    }), false, field);
  }
});
