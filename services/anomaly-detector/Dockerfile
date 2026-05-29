# =============================================================================
#  anomaly-detector — Python FastAPI + scikit-learn IsolationForest.
# =============================================================================
FROM python:3.12-slim AS base
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1
WORKDIR /app

# scikit-learn pulls libgomp; numpy compiles fine with the slim image.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgomp1 \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt ./
RUN pip install -r requirements.txt

COPY anomaly_detector ./anomaly_detector

RUN useradd --uid 65532 --no-create-home --shell /usr/sbin/nologin app
USER 65532:65532

EXPOSE 8095
CMD ["uvicorn", "anomaly_detector.app:app", "--host", "0.0.0.0", "--port", "8095"]
