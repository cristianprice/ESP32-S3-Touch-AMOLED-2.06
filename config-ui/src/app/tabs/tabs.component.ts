import { Component, input, output, signal } from '@angular/core';

export interface Tab {
  id: string;
  label: string;
}

@Component({
  selector: 'app-tabs',
  templateUrl: './tabs.component.html',
  styleUrl: './tabs.component.scss',
})
export class TabsComponent {
  readonly tabs = input.required<Tab[]>();
  readonly tabChange = output<string>();
  protected readonly activeTab = signal<string | null>(null);

  protected selectTab(tabId: string): void {
    this.activeTab.set(tabId);
    this.tabChange.emit(tabId);
  }

  protected isActive(tabId: string): boolean {
    return this.activeTab() === null ? tabId === this.tabs()[0]?.id : this.activeTab() === tabId;
  }
}
