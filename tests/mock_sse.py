#!/usr/bin/env python3
"""Local stand-in for the Anthropic, chat, and responses streams."""

import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer


def anthropic(messages):
    last = messages[-1] if messages else {}
    content = last.get("content")
    has_result = isinstance(content, list) and any(
        isinstance(block, dict) and block.get("type") == "tool_result" for block in content
    )
    if not has_result:
        return [
            ("content_block_start", '{"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"t1","name":"bash","input":{}}}'),
            ("content_block_delta", '{"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\\"command\\":\\"echo beige-ok\\"}"}}'),
            ("content_block_stop", '{"type":"content_block_stop","index":0}'),
            ("message_delta", '{"type":"message_delta","delta":{"stop_reason":"tool_use"}}'),
            ("message_stop", '{"type":"message_stop"}'),
        ]
    return [
        ("content_block_start", '{"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}'),
        ("content_block_delta", '{"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"done"}}'),
        ("content_block_stop", '{"type":"content_block_stop","index":0}'),
        ("message_delta", '{"type":"message_delta","delta":{"stop_reason":"end_turn"}}'),
        ("message_stop", '{"type":"message_stop"}'),
    ]


def chat(messages):
    has_tool = any(isinstance(m, dict) and m.get("role") == "tool" for m in messages)
    if not has_tool:
        return (
            'data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"c1","function":{"name":"bash","arguments":""}}]}}]}\n\n'
            'data: {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\\"command\\":\\"echo beige-ok\\"}"}}]}}]}\n\n'
            'data: {"choices":[{"finish_reason":"tool_calls"}]}\n\n'
            "data: [DONE]\n\n"
        )
    return (
        'data: {"choices":[{"delta":{"content":"done"}}]}\n\n'
        'data: {"choices":[{"finish_reason":"stop"}]}\n\n'
        "data: [DONE]\n\n"
    )


def responses(items):
    has_out = any(isinstance(item, dict) and item.get("type") == "function_call_output" for item in items)
    if not has_out:
        return (
            'event: response.output_item.added\n'
            'data: {"type":"response.output_item.added","item":{"type":"function_call","id":"fc_1","call_id":"call_1","name":"bash","arguments":""}}\n\n'
            'event: response.function_call_arguments.delta\n'
            'data: {"type":"response.function_call_arguments.delta","item_id":"fc_1","delta":"{\\"command\\":\\"echo beige-ok\\"}"}\n\n'
            'event: response.completed\n'
            'data: {"type":"response.completed","response":{"usage":{"input_tokens":1,"output_tokens":1}}}\n\n'
        )
    return (
        'event: response.output_text.delta\n'
        'data: {"type":"response.output_text.delta","delta":"done"}\n\n'
        'event: response.completed\n'
        'data: {"type":"response.completed"}\n\n'
    )


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = json.dumps(
            {
                "object": "list",
                "has_more": False,
                "data": [
                    {"id": "claude-from-api", "object": "model"},
                    {"id": "whisper-1", "object": "model"},
                    {"id": "claude-other", "object": "model"},
                ],
            }
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError:
            body = {}
        path = self.path
        if path.endswith("/chat/completions"):
            payload = chat(body.get("messages") or [])
        elif path.endswith("/responses"):
            payload = responses(body.get("input") or [])
        else:
            events = anthropic(body.get("messages") or [])
            payload = "".join("event: %s\ndata: %s\n\n" % pair for pair in events)
        data = payload.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        return


def main():
    port = int(sys.argv[1])
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
