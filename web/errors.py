class AppError(Exception):
    def __init__(self, message, code=400):
        self.message, self.code = message, code
