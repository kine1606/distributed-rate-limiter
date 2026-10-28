"""Payments (Phase 2, P3): the platform never sees a card number. The card is typed into the payment
provider's own form, which hands back a token; the platform passes the token on, keeps only the
provider's customer id ({rl:m:<M>}:payers) and charges that id when an invoice closes. Each charge
carries the invoice as its idempotency key, so closing a month again never charges twice.

SandboxProvider stands in for a provider's test mode (docs/PRODUCT_PLAN.md section 10: Stripe test
mode, a VNPay sandbox, or invoices only, still to settle with the advisor). It accepts Stripe's test
tokens and keeps its state under {rl:sandbox}: what the provider would store, not the platform."""
import json
import re
import secrets

TOKEN_RE = re.compile(r"tok_[A-Za-z0-9_]{1,64}")
CARD_NUMBER_RE = re.compile(r"\d(?:[ -]?\d){11,18}")  # 12 to 19 digits, maybe grouped: a card number

CUSTOMERS = "{rl:sandbox}:customers"  # provider customer id -> {"reference", "card"}
CHARGES = "{rl:sandbox}:charges"      # idempotency key -> the charge


class PaymentError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def check_token(token):
    """Refuses anything but a provider token; above all, a card number."""
    if CARD_NUMBER_RE.search(token):
        raise PaymentError("card_number_refused", "send the payment provider's token, never a card number")
    if not TOKEN_RE.fullmatch(token):
        raise PaymentError("invalid_token", "a payment token looks like tok_...")


class SandboxProvider:
    name = "sandbox"
    # Stripe's test tokens: brand, last four digits, and how a charge to it fails (None: it succeeds).
    TEST_CARDS = {
        "tok_visa": ("visa", "4242", None),
        "tok_mastercard": ("mastercard", "4444", None),
        "tok_chargeDeclined": ("visa", "0002", "card_declined"),
        "tok_chargeDeclinedInsufficientFunds": ("visa", "9995", "insufficient_funds"),
    }

    def __init__(self, redis):
        self.r = redis

    def create_customer(self, reference):
        pid = "cus_" + secrets.token_hex(8)
        self.r.hset(CUSTOMERS, pid, json.dumps({"reference": reference, "card": None}))
        return pid

    def attach(self, pid, token):
        """Makes `token`'s card the customer's; returns its brand and last four digits."""
        check_token(token)
        if token not in self.TEST_CARDS:
            raise PaymentError("invalid_token", "the sandbox knows only its test tokens: " + ", ".join(self.TEST_CARDS))
        stored = self.r.hget(CUSTOMERS, pid)
        if stored is None:
            raise PaymentError("no_such_customer", "the provider has no such customer")
        self.r.hset(CUSTOMERS, pid, json.dumps({**json.loads(stored), "card": token}))
        return self.card(pid)

    def card(self, pid):
        stored = self.r.hget(CUSTOMERS, pid)
        token = json.loads(stored)["card"] if stored else None
        if token is None:
            return None
        brand, last4, _ = self.TEST_CARDS[token]
        return {"brand": brand, "last4": last4}

    def charge(self, pid, amount, currency, idempotency_key):
        """{"id", "ok", "failure", "amount", "currency"}; the same key returns the first result."""
        if (first := self.r.hget(CHARGES, idempotency_key)) is not None:
            return json.loads(first)
        stored = self.r.hget(CUSTOMERS, pid)
        token = json.loads(stored)["card"] if stored else None
        failure = "no_card" if token is None else self.TEST_CARDS[token][2]
        result = {"id": "ch_" + secrets.token_hex(8), "ok": failure is None, "failure": failure, "amount": amount,
                  "currency": currency}
        if not self.r.hsetnx(CHARGES, idempotency_key, json.dumps(result)):  # a concurrent charge won
            return json.loads(self.r.hget(CHARGES, idempotency_key))
        return result
