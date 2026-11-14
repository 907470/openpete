#!/usr/bin/env bash
#
# scratch_cards.sh - the scratch memory cards of a harness, now part of its
# sandbox. SOURCED, not executed; sourcing it does the work.
#
# Sources scripts/lib/sandbox.sh, which exports OPENPETE_SANDBOX as a fresh
# root (every per-user directory of the game resolves under it, the cards
# among them) and OPENPETE_CARD_DIR as <root>/cards. A run that wants a card
# keeps it: --card1, --card2 and --card-dir beat the variable, and --no-card
# removes both cards. op_scratch_cards_cleanup, called from a script's own
# EXIT trap, removes the root.

# shellcheck source=scripts/lib/sandbox.sh
source "$(dirname "${BASH_SOURCE[0]}")/sandbox.sh"

op_scratch_cards_cleanup() {
    op_sandbox_cleanup
}
