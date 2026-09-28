// Helpers for the New Machine dialog's Machine Model select, whose options
// are model/ROM pairs: the value is the model id, or model/romId when several
// ROMs boot that model, and each option carries
// its model id as data-model.

// The value of the first option for `model` ('' when there is none).
export function modelValue(sel: HTMLSelectElement, model: string): string {
  return Array.from(sel.options).find((o) => o.dataset.model === model)?.value ?? '';
}

// Whether the select offers `model` at all.
export function hasModel(sel: HTMLSelectElement | null, model: string): boolean {
  return !!sel && Array.from(sel.options).some((o) => o.dataset.model === model);
}

// The model id of the selected option ('' when nothing is selected).
export function selectedModel(sel: HTMLSelectElement): string {
  return sel.selectedOptions[0]?.dataset.model ?? '';
}
