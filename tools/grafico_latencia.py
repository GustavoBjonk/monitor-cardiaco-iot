"""Gera a tabela e o gráfico de tempo de resposta a partir do CSV exportado pelo dashboard.

Uso:
    python tools/grafico_latencia.py medicoes_latencia.csv [saida.png]

Colunas esperadas no CSV: n, componente, rtt_ms, esp32_ms, tempo_resposta_ms, horario
"""
import csv
import sys
from collections import OrderedDict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def carregar(caminho):
    grupos = OrderedDict()
    with open(caminho, newline="", encoding="utf-8") as f:
        for linha in csv.DictReader(f):
            grupos.setdefault(linha["componente"], []).append(
                {
                    "rtt": float(linha["rtt_ms"]),
                    "esp": float(linha["esp32_ms"]),
                    "resp": float(linha["tempo_resposta_ms"]),
                }
            )
    return grupos


def fmt(v):
    return f"{v:.1f}".replace(".", ",")


def imprimir_tabela(grupos):
    print("| Nº medida | Sensor/atuador | Tempo de resposta (ms) |")
    print("|---|---|---|")
    for comp, medidas in grupos.items():
        for i, m in enumerate(medidas, 1):
            print(f"| {i} | {comp} | {fmt(m['resp'])} |")
        media = sum(m["resp"] for m in medidas) / len(medidas)
        print(f"| | **Média — {comp}** | **{fmt(media)}** |")


def grafico(grupos, saida):
    fig, eixos = plt.subplots(1, len(grupos), figsize=(5 * len(grupos), 3.6), squeeze=False)
    for ax, (comp, medidas) in zip(eixos[0], grupos.items()):
        xs = list(range(1, len(medidas) + 1))
        rede = [m["rtt"] / 2 for m in medidas]
        esp = [m["esp"] for m in medidas]
        ax.bar(xs, rede, color="#4c6ef5", label="rede (RTT/2)")
        ax.bar(xs, esp, bottom=rede, color="#c8102e", label="processamento no ESP32")
        media = sum(m["resp"] for m in medidas) / len(medidas)
        ax.axhline(media, color="#333", ls="--", lw=1)
        ax.text(len(xs) + 0.45, media, f"média\n{fmt(media)} ms", va="center", fontsize=8)
        for x, m in zip(xs, medidas):
            ax.text(x, m["resp"] + 8, fmt(m["resp"]), ha="center", fontsize=8)
        ax.set_title(comp, fontsize=10)
        ax.set_xlabel("Medição")
        ax.set_ylabel("Tempo de resposta (ms)")
        ax.set_xticks(xs)
        ax.set_xlim(0.4, len(xs) + 1.3)
        ax.set_ylim(0, max(m["resp"] for m in medidas) * 1.25)
        ax.spines[["top", "right"]].set_visible(False)
    eixos[0][0].legend(fontsize=8, loc="lower left")
    fig.tight_layout()
    fig.savefig(saida, dpi=200)
    print(f"\nGráfico salvo em {saida}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    dados = carregar(sys.argv[1])
    imprimir_tabela(dados)
    grafico(dados, sys.argv[2] if len(sys.argv) > 2 else "latencia.png")
