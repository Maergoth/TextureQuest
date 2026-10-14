"""Cumulative byte and archive progress for an explicit pack operation."""


class OverallProgress:
    def __init__(self, rows, callback=None):
        self.callback = callback or (lambda *_: None)
        self.total_bytes = sum(row["bytes"] for row in rows)
        self.total_files = len(rows)
        self.completed_bytes = self.completed_files = self.current_bytes = 0
        self.current = None
        self.emit("Preparing files…")

    def emit(self, message):
        self.callback(message, self.completed_bytes + self.current_bytes,
                      self.total_bytes, self.completed_files, self.total_files)

    def start(self, row, message):
        self.current = row
        self.current_bytes = 0
        self.emit(message)

    def update(self, done_bytes, message):
        if self.current is not None:
            self.current_bytes = max(self.current_bytes, min(max(0, done_bytes), self.current["bytes"]))
            self.emit(message)

    def complete(self, message):
        self.completed_bytes += self.current["bytes"]
        self.completed_files += 1
        self.current_bytes = 0
        self.current = None
        self.emit(message)
