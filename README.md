# Fill-Simulator

An event-driven backtester for market-making strategies. It replays recorded exchange data, lets a pluggable strategy place, cancel and replace orders, models latency, decides when those orders would have been filled, and reports P&L and fill statistics.

## Simulation modes

Selected by `use_queue_simulation` in the TOML config. The mode only changes how market data is read; fills are decided the same way in both (see below).

| Mode                      | Input                                         | Market data fed to the strategy |
|---------------------------|-----------------------------------------------|---------------------------------|
| Tops/fills (`false`)      | `book_tops` + `book_fills` files              | Tops and fills are replayed in timestamp order. Each top updates the market state and is passed to the strategy; each recorded trade is passed to the strategy's `onFill`. |
| Queue simulation (`true`) | `book_events` file (full order-by-order feed) | The market's book is rebuilt from add/delete/replace/amend/reduce/execute/clear events, with a FIFO queue per price level. The top three levels are recomputed after each event and passed to the strategy as a top, and each execute event is passed to `onFill` as a trade. |

Your own orders are never inserted into the rebuilt book or its queues. They are tracked separately and filled by the rule below.

### How fills are decided

- After each valid top, every resting order whose price is marketable against the new best quote is filled for its remaining quantity: a bid when its price is at or above the best ask, an ask when its price is at or below the best bid. The fill price is the opposite best quote.
- A new or replaced order that is already marketable when it arrives is cancelled if it is post-only; otherwise it fills at the opposite best quote.
- Tops are skipped if they arrive less than 100 µs after the last processed top, if either side is empty or non-positive, if the book is crossed, or if a price exceeds $10,000.

## Latency model

Two configurable delays are applied to every order's round trip:

- `strategy_md_latency_ns`: added to every top and trade before the strategy sees it.
- `exchange_latency_ns`: one-way delay between strategy and exchange. It is added to an order action's arrival time at the exchange and again to the fill notification sent back.

The total round trip is `md + 2 * exchange`. The run prints average latencies per stage at the end.

## Strategies

Chosen interactively at startup. All implement `Strategy` (`strategies/strategy.h`): `onBookTopUpdate`, `onFill`, `onOrderFilled`, returning a list of `ADD` / `CANCEL` / `REPLACE` actions. Every strategy quotes a quantity of 1 and cancels orders that have rested for 10 minutes.

| # | Strategy    | Behavior |
|---|-------------|----------|
| 1 | Basic       | When the top of book changes (at most once per 10 minutes), buys 1 at the best bid and sells 1 at the best ask. Orders are not post-only. |
| 2 | Theo        | Theo is 0.7 x an exponentially weighted average of the last 100 trades (decay 0.05; market trades and own fills) + 0.3 x the mid, or just the mid before any trade is seen. Quotes a post-only bid at `theo * (1 - place%)` and ask at `theo * (1 + place%)`, as long as they sit inside the market, and re-quotes when a price moves by more than 0.1% of theo. A bid is cancelled when `(theo - bid) / theo` falls below `cancel_edge_percent`; an ask likewise. |
| 3 | Correlation | Theo is built from the symbol's own mid, time-weighted over the last 60 s (up to 20 points), plus the latest mids of up to the 10 symbols most correlated with it (by absolute correlation in the CSV). Each correlated symbol is weighted `(1 - self_weight) * abs(correlation)`, and a negatively correlated symbol enters mirrored around the symbol's own mid. Quotes post-only bid and ask at theo -/+ `place_edge_percent`, rounded to the tick and kept from crossing the market, and only re-quotes when theo moves by at least 0.01%. A bid is cancelled when its price is above `theo * (1 - cancel_edge_percent)` or below the best bid; an ask is the mirror. |

If `cancel_edge_percent` is not below `place_edge_percent`, the Theo strategy sets cancel to 80% of place.

The Correlation strategy prompts for the correlation CSV (produced by [market-data-processing](../market-data-processing)) and a symbol-mapping CSV with header `stock_locate,symbol`. It then loads the correlated symbols' data files from the same directory as the main input file, named `<EXCHANGE>.book_events.<SYMBOL>.bin` (queue mode) or `<EXCHANGE>.book_tops.<SYMBOL>.bin` and `.book_fills.<SYMBOL>.bin`, and replays them alongside the main symbol. In tops/fills mode the input path is not passed to the strategy, so it prompts for the main symbol's data file path as well.

To add a strategy, subclass `Strategy`, add a source file under `strategies/` (the Makefile picks up `strategies/*.cpp`), and register it in `createStrategy` in `main.cpp`.

## Build

Requires a C++17 compiler. [toml11](https://github.com/ToruNiina/toml11) is downloaded to `externals/toml11/` on the first build if missing.

```bash
make            # builds bin/fill_simulator
make clean
```

## Usage

```bash
# Tops/fills mode
./bin/fill_simulator <book_tops_file> <book_fills_file> <output_file> <config.toml>

# Queue simulation mode
./bin/fill_simulator <book_events_file> <output_file> <config.toml>
```

The program prints the strategy menu and reads your choice from stdin. To script it, pipe the number in, for example `echo 2 | ./bin/fill_simulator ...` (this is how [optimal-edge-fitter](../optimal-edge-fitter) drives it).

## Configuration

`latencies/latency_config_no_queue.toml` (tops/fills) and `latencies/latency_config_queue.toml` (queue) differ only in `use_queue_simulation`.

```toml
[latency]
strategy_md_latency_ns = 1000      # 1 µs
exchange_latency_ns    = 10000     # 10 µs

[simulation]
use_queue_simulation = false

[strategy]
place_edge_percent  = 0.01         # percent of theo
cancel_edge_percent = 0.005
self_weight         = 0.5          # correlation strategy, 0.0-1.0
```

If the file is missing or fails to parse, built-in defaults are used.

## Output

- A binary order-event log at `<output_file>`: raw `OrderRecord` structs (native alignment, no header) with `timestamp`, `event_type` (1 = add, 2 = cancel, 3 = fill, 4 = replace), `order_id`, `symbol_id`, `price`, `old_price`, `quantity`, `old_quantity` and `is_bid`. The old values are only set on replaces.
- A results block on stdout: latency statistics, then strategy name, orders placed and filled, fill rate, buy and sell volume and cost, final position, final mid, final P&L, and average buy price, sell price and spread captured. P&L is cash flow plus the open position marked at the last valid mid.

Other tools parse the `Final P&L: $...` line, so keep that format if you change the output.

## Layout

```
main.cpp                config loading, strategy selection, run
fill_simulator.{h,cpp}  simulation engine, queue model, latency, results
strategies/             Strategy interface + basic, theo, correlation
types/                  packed binary market-data structs
latencies/              example TOML configs
externals/toml11/       TOML parser (single header)
```

Prices are integers in nanos (1e-9 dollars) throughout.
