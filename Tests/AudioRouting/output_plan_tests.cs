using System;
using RSMods.Audio;

namespace RSMods.Tests
{
	// Pure decision coverage for OutputApplyPlan: which "Apply output" presses become a live, no-restart
	// route move versus falling through to the existing arm/install flow. No game, no I/O.
	//
	// The signature is Plan(asioMode, gameRunning, connected, proxyChainLive, isPassthrough, isRouting,
	//                       chosenIsAsioDevice, chosenIsRoutedDevice, proxyNeedsPromotion).
	internal static class OutputPlanTests
	{
		private static int Main()
		{
			try
			{
				// ---- ASIO input mode: the proxy is the live chain, live route moves are possible ----

				// On ASIO M-Track, game up, passthrough, pick a plain Windows device -> route it live (op 21).
				Expect(OutputApplyPlan.Plan(true, true, true, true, true, false, false, false, false),
					OutputApplyAction.RouteToDevice, "ASIO live: non-ASIO device while passthrough routes live");

				// Already routing from the real ASIO callback; pick the ASIO device -> stop the route (back on ASIO), live.
				Expect(OutputApplyPlan.Plan(true, true, true, true, true, true, true, false, false),
					OutputApplyAction.StopRoute, "ASIO live: ASIO device while routing stops the route");

				// A virtual proxy can keep feeding the route after the real ASIO callback stalls. It must be
				// promoted before the route is stopped, otherwise op 22 leaves the game on silent virtual output.
				Expect(OutputApplyPlan.Plan(true, true, true, true, true, true, true, false, true),
					OutputApplyAction.PromoteToAsio, "ASIO live: virtual route promotes before stopping the route");

				// Picking the device already being routed to is a no-op live move.
				Expect(OutputApplyPlan.Plan(true, true, true, true, true, true, false, true, false),
					OutputApplyAction.UseExistingFlow, "ASIO live: re-picking the routed device does nothing live");

				// Retarget: routing to A, pick a different non-ASIO device B -> route to B live.
				Expect(OutputApplyPlan.Plan(true, true, true, true, true, true, false, false, false),
					OutputApplyAction.RouteToDevice, "ASIO live: different non-ASIO device while routing retargets");

				// ASIO device while NOT routing: an explicit Apply re-binds the driver (the recovery for a
				// real device that is dead but still calling back).
				Expect(OutputApplyPlan.Plan(true, true, true, true, true, false, true, false, false),
					OutputApplyAction.RebindAsio, "ASIO live: Apply on the bound ASIO device re-binds it");
				Expect(OutputApplyPlan.Plan(true, true, true, true, true, false, true, false, true),
					OutputApplyAction.PromoteToAsio, "ASIO live: explicit ASIO selection promotes a virtual proxy");

				// Cable mode owns a full shared-output session. Even ASIO-capable hardware stays on the
				// ordinary live WASAPI switch path while RS_ASIO is disabled.
				Expect(OutputApplyPlan.Plan(false, true, true, true, true, false, false, false, false),
					OutputApplyAction.UseExistingFlow, "Cable mode bypasses the ASIO proxy route path");
				Expect(OutputApplyPlan.Plan(false, true, true, true, true, false, true, false, true),
					OutputApplyAction.UseExistingFlow, "Cable mode does not promote ASIO-capable output hardware");

				// ---- Preconditions: any missing gate falls through, in either mode ----
				Expect(OutputApplyPlan.Plan(true, false, true, true, true, false, false, false, false),
					OutputApplyAction.UseExistingFlow, "game closed cannot route live");
				Expect(OutputApplyPlan.Plan(true, true, false, true, true, false, false, false, false),
					OutputApplyAction.UseExistingFlow, "no control connection cannot route live");
				Expect(OutputApplyPlan.Plan(true, true, true, false, true, false, false, false, false),
					OutputApplyAction.UseExistingFlow, "no live ASIO chain falls through to the WASAPI flow");
				Expect(OutputApplyPlan.Plan(true, true, true, true, false, false, false, false, false),
					OutputApplyAction.UseExistingFlow, "a full WASAPI session is not the route path");

				Console.WriteLine("PASS: Cable mode stays on live shared output; only ASIO mode uses proxy routing and promotion");
				return 0;
			}
			catch (Exception error)
			{
				Console.WriteLine("FAIL: " + error.Message);
				return 1;
			}
		}

		private static void Expect(OutputApplyAction actual, OutputApplyAction expected, string what)
		{
			if (actual != expected) throw new Exception(what + " (expected " + expected + ", got " + actual + ")");
		}
	}
}
