"""
BareStats Universal Telemetry Tracker (pip install barestats)
Zero-dependency background metrics exporter for BareStats (Port 38111).
Supports Python 3.8+. Compatible with:
- llama.cpp (llama-cpp-python)
- Hugging Face Transformers (model.generate streamer & pipelines)
- Ollama Python SDK (ollama.chat / ollama.generate)
- OpenAI / vLLM / LM Studio / LiteLLM Python SDKs
- ExLlamaV2 / ONNX Runtime GenAI / Custom PyTorch loops
"""

import atexit
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler
from contextlib import contextmanager
from typing import Any, Dict, Iterable, Iterator, Optional

__version__ = "1.4.0"
DEFAULT_BARESTATS_PORT = 38111


def _normalize_content(value: Any) -> str:
    """Normalize chunk content to a plain string."""
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return str(value)


def _model_name_from_llama(llm: Any, default: str = "llama.cpp") -> str:
    """Best-effort model name extraction from a llama-cpp-python Llama object."""
    for attr in (
        "model_name",
        "model_path",
        "model_path_str",
        "model_file",
        "gguf_path",
        "file",
        "path",
    ):
        value = getattr(llm, attr, None)
        if isinstance(value, str) and value:
            name = value.replace("\\", "/").rsplit("/", 1)[-1].strip()
            if name:
                return name

    try:
        metadata = llm.metadata()
        if isinstance(metadata, dict):
            general = metadata.get("general") or {}
            if isinstance(general, dict):
                for key in ("name", "architecture"):
                    if general.get(key):
                        return str(general[key])
    except Exception:
        pass

    return default


def _estimate_llama_cpp_prompt_tokens(llm: Any, messages: Any) -> int:
    """Estimate prompt tokens for a llama-cpp-python chat completion request."""
    try:
        prompt = llm.format_chat_prompt(
            messages,
            add_bos=True,
            add_eos=False,
        )
        tokens = llm.tokenize(prompt, add_bos=False, special=True)
        if tokens is not None:
            return len(tokens)
    except Exception:
        pass

    try:
        message_list = list(messages)
    except TypeError:
        message_list = []

    text = " ".join(
        _normalize_content(message.get("content"))
        for message in message_list
        if isinstance(message, dict)
    )

    try:
        tokens = llm.tokenize(text, add_bos=True, special=True)
        if tokens is not None:
            return len(tokens)
    except Exception:
        pass

    return max(1, len(text.split()))


class _ReusableHTTPServer(HTTPServer):
    """HTTP Server that immediately reuses socket address without TIME_WAIT delays."""
    allow_reuse_address = True


class BareStatsTracker:
    """
    Lightweight, near-zero-overhead metrics server exposing Prometheus /metrics
    on 127.0.0.1:38111 for BareStats LLM telemetry scrapers.
    """

    def __init__(
        self,
        model_name: str = "custom-llm",
        port: int = DEFAULT_BARESTATS_PORT,
        auto_start: bool = False,
    ):
        self.model_name = str(model_name)
        self.port = int(port)
        self.gen_tokens = 0
        self.prompt_tokens = 0
        self.is_thinking = False
        self.active_requests = 0
        self.kv_cache_pct = 0.0
        self._thinking_pulse = 0
        self._server: Optional[HTTPServer] = None
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._running = False
        if auto_start:
            self.start()

    def start(self) -> "BareStatsTracker":
        """Starts the background telemetry HTTP server daemon."""
        with self._lock:
            if self._running:
                return self

            tracker = self

            class PrometheusMetricsHandler(BaseHTTPRequestHandler):
                def do_GET(self):
                    if self.path in ("/metrics", "/"):
                        with tracker._lock:
                            if tracker.is_thinking:
                                tracker._thinking_pulse += 1
                            gen = tracker.gen_tokens
                            prompt = tracker.prompt_tokens + tracker._thinking_pulse
                            model = tracker.model_name
                            running = tracker.active_requests if tracker.active_requests > 0 else (1 if (tracker.is_thinking or tracker.gen_tokens > 0) else 0)
                            kv_pct = tracker.kv_cache_pct

                        body = (
                            f"# HELP vllm:generation_tokens_total Cumulative generated tokens\n"
                            f"# TYPE vllm:generation_tokens_total counter\n"
                            f'vllm:generation_tokens_total{{model="{model}"}} {gen}\n'
                            f"# HELP vllm:prompt_tokens_total Cumulative prompt tokens\n"
                            f"# TYPE vllm:prompt_tokens_total counter\n"
                            f'vllm:prompt_tokens_total{{model="{model}"}} {prompt}\n'
                            f"# HELP vllm:num_requests_running Number of currently active requests\n"
                            f"# TYPE vllm:num_requests_running gauge\n"
                            f'vllm:num_requests_running{{model="{model}"}} {running}\n'
                            f"# HELP vllm:kv_cache_usage_perc Current KV cache utilization percentage\n"
                            f"# TYPE vllm:kv_cache_usage_perc gauge\n"
                            f'vllm:kv_cache_usage_perc{{model="{model}"}} {kv_pct:.2f}\n'
                        ).encode("utf-8")

                        self.send_response(200)
                        self.send_header(
                            "Content-Type", "text/plain; version=0.0.4; charset=utf-8"
                        )
                        self.send_header("Content-Length", str(len(body)))
                        self.end_headers()
                        self.wfile.write(body)
                    else:
                        self.send_response(404)
                        self.end_headers()

                def log_message(self, format: str, *args: Any) -> None:
                    # Suppress console logging to keep application terminal clean
                    pass

            try:
                self._server = _ReusableHTTPServer(
                    ("127.0.0.1", self.port), PrometheusMetricsHandler
                )
            except OSError:
                # Port already in use or permission denied; fail gracefully without crashing
                return self

            self._running = True
            self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
            self._thread.start()
            atexit.register(self.stop)
            return self

    def stop(self) -> None:
        """Shuts down the background HTTP server."""
        with self._lock:
            if self._server and self._running:
                self._running = False
                try:
                    self._server.shutdown()
                    self._server.server_close()
                except Exception:
                    pass
                self._server = None
                self._thread = None
                try:
                    atexit.unregister(self.stop)
                except Exception:
                    pass

    def step(self, tokens: int = 1) -> None:
        """Increment generated tokens counter by N."""
        with self._lock:
            self.gen_tokens += tokens
            self.is_thinking = False

    def set_prompt_tokens(self, count: int) -> None:
        """Set or increment prompt tokens count."""
        with self._lock:
            self.prompt_tokens += count

    def set_thinking(self, thinking: bool = True) -> None:
        """Set thinking state (prompt evaluation / prefill phase)."""
        with self._lock:
            self.is_thinking = thinking

    def set_model(self, model_name: str) -> None:
        """Dynamically update model name if the script swaps weights at runtime."""
        with self._lock:
            self.model_name = str(model_name)

    def set_kv_cache(self, pct: float) -> None:
        """Update KV cache utilization percentage (0.0 to 100.0)."""
        with self._lock:
            self.kv_cache_pct = max(0.0, min(100.0, float(pct)))

    @staticmethod
    def _extract_chunk_text(chunk: Any) -> Optional[str]:
        """
        Universal text extractor supporting OpenAI SDK, Ollama, llama-cpp-python,
        vLLM, DeepSeek reasoning content, and raw strings.
        """
        if chunk is None:
            return None
        if isinstance(chunk, str):
            return chunk
        if isinstance(chunk, bytes):
            return chunk.decode("utf-8", errors="replace")

        # 1. Dict-like chunk (llama-cpp-python, ollama dict, raw JSON)
        if isinstance(chunk, dict):
            # DeepSeek / reasoning content check
            choices = chunk.get("choices")
            if choices and isinstance(choices, list) and len(choices) > 0:
                first = choices[0]
                if isinstance(first, dict):
                    delta = first.get("delta") or {}
                    if isinstance(delta, dict):
                        if delta.get("reasoning_content") or delta.get("thinking"):
                            return "__THINKING__"
                        content = delta.get("content")
                        if content is not None:
                            return content if isinstance(content, str) else str(content)
                    if first.get("text"):
                        return str(first["text"])

            # Ollama chat dict
            msg = chunk.get("message")
            if isinstance(msg, dict):
                if msg.get("thinking"):
                    return "__THINKING__"
                content = msg.get("content")
                if content is not None:
                    return content if isinstance(content, str) else str(content)

            # Ollama generate dict
            if "response" in chunk:
                resp = chunk["response"]
                return resp if isinstance(resp, str) else str(resp)

        # 2. Object with attributes (OpenAI SDK ChatCompletionChunk, Ollama ChatResponse object)
        try:
            if hasattr(chunk, "choices"):
                choices = getattr(chunk, "choices", None)
                if choices and len(choices) > 0:
                    delta = getattr(choices[0], "delta", None)
                    if delta is not None:
                        if getattr(delta, "reasoning_content", None) or getattr(delta, "thinking", None):
                            return "__THINKING__"
                        content = getattr(delta, "content", None)
                        if content is not None:
                            return str(content)
                    text = getattr(choices[0], "text", None)
                    if text is not None:
                        return str(text)

            if hasattr(chunk, "message"):
                msg = getattr(chunk, "message", None)
                if msg is not None:
                    if getattr(msg, "thinking", None):
                        return "__THINKING__"
                    content = getattr(msg, "content", None)
                    if content is not None:
                        return str(content)

            if hasattr(chunk, "response"):
                resp = getattr(chunk, "response", None)
                if resp is not None:
                    return str(resp)
        except Exception:
            pass

        return None

    def track_stream(self, stream: Iterable[Any], prompt_tokens: int = 0) -> Iterator[Any]:
        """
        Universal wrapper for any streaming token generator.
        Increments token count on each yielded item, tracks prompt prefill,
        and automatically recognizes DeepSeek reasoning phases.
        """
        self.start()
        with self._lock:
            if prompt_tokens > 0:
                self.prompt_tokens += prompt_tokens
            self.is_thinking = True
            self.active_requests += 1

        try:
            for chunk in stream:
                text = self._extract_chunk_text(chunk)
                if text == "__THINKING__":
                    with self._lock:
                        self.is_thinking = True
                        self._thinking_pulse += 1
                elif text:
                    self.step(1)
                yield chunk
        finally:
            with self._lock:
                self.is_thinking = False
                self.active_requests = max(0, self.active_requests - 1)

    def stream_llama_cpp_chat(
        self,
        llm: Any,
        messages: Any,
        *,
        model_name: Optional[str] = None,
        **kwargs: Any,
    ) -> Iterator[str]:
        """
        Track a llama-cpp-python chat completion stream.
        Yields assistant content strings directly while updating BareStats telemetry.
        """
        self.start()
        chosen_model = model_name or kwargs.pop("model_name", None)
        if chosen_model:
            self.set_model(chosen_model)
        else:
            self.set_model(_model_name_from_llama(llm, self.model_name))

        kwargs.pop("model_name", None)
        prompt_tokens = _estimate_llama_cpp_prompt_tokens(llm, messages)
        kwargs["stream"] = True
        stream = llm.create_chat_completion(messages=messages, **kwargs)

        for chunk in self.track_stream(stream, prompt_tokens=prompt_tokens):
            text = self._extract_chunk_text(chunk)
            if text and text != "__THINKING__":
                yield text

    def hf_streamer(self) -> Any:
        """
        Returns a Hugging Face `model.generate(..., streamer=tracker.hf_streamer())`
        compatible streamer object that reports live token generation to BareStats
        without requiring any extra dependencies.
        """
        tracker = self
        tracker.start()

        class _HFStreamer:
            def __init__(self) -> None:
                self._first = True
                with tracker._lock:
                    tracker.is_thinking = True
                    tracker.active_requests += 1

            def put(self, value: Any) -> None:
                # Hugging Face passes prompt input_ids on the very first put() call
                if self._first:
                    self._first = False
                    try:
                        prompt_len = value.shape[-1] if hasattr(value, "shape") else len(value)
                    except Exception:
                        prompt_len = 1
                    tracker.set_prompt_tokens(max(1, int(prompt_len)))
                    return
                tracker.step(1)

            def end(self) -> None:
                with tracker._lock:
                    tracker.is_thinking = False
                    tracker.active_requests = max(0, tracker.active_requests - 1)

        return _HFStreamer()

    @contextmanager
    def generate(self, prompt_tokens: int = 0):
        """
        Context manager for non-streaming batch execution.
        Usage:
            with tracker.generate(prompt_tokens=len(input_ids[0])) as gen:
                outputs = model.generate(**inputs, max_new_tokens=150)
                gen["tokens"] = len(outputs[0]) - len(input_ids[0])
        """
        self.start()
        with self._lock:
            self.prompt_tokens += max(1, prompt_tokens)
            self.is_thinking = True
            self.active_requests += 1
        state: Dict[str, int] = {"tokens": 0}
        try:
            yield state
        finally:
            with self._lock:
                self.gen_tokens += max(0, int(state.get("tokens", 0)))
                self.is_thinking = False
                self.active_requests = max(0, self.active_requests - 1)


__all__ = ["BareStatsTracker", "DEFAULT_BARESTATS_PORT"]
