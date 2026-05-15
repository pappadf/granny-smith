<script lang="ts">
  // Phase 0 placeholder. Real components arrive in Phase 1 (Workbench,
  // DisplayToolbar, Panel, StatusBar). This component only verifies that
  // tokens.css is loaded and the dark theme is applied.
</script>

<main class="placeholder">
  <h1>Granny Smith</h1>
  <p class="tagline">Phase 0 scaffold — components arrive in Phase 1.</p>
</main>

<style>
  .placeholder {
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    height: 100vh;
    gap: 0.5rem;
    color: var(--gs-fg-bright);
  }

  h1 {
    margin: 0;
    font-size: 1.5rem;
    font-weight: 500;
    letter-spacing: 0.02em;
  }

  .tagline {
    margin: 0;
    color: var(--gs-fg-muted);
    font-size: 0.85rem;
  }
</style>
