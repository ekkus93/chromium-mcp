import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import Ajv from 'ajv';

const schema = JSON.parse(
  await readFile(new URL('./browser_agent_protocol.schema.json', import.meta.url), 'utf8'),
);
const validate = new Ajv({ allErrors: true }).compile(schema);

const requestMeta = { 'io.modelcontextprotocol/protocolVersion': '2026-07-28' };

const validMessages = [
  { jsonrpc: '2.0', id: 1, method: 'server/discover', params: { _meta: requestMeta } },
  { jsonrpc: '2.0', id: 'list-1', method: 'tools/list', params: { _meta: requestMeta } },
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
});
